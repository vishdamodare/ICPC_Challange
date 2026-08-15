#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <iomanip>
#include <string>
#include <algorithm>
#include <sstream>
#include <cassert>

struct SimScenario {
    int id;
    std::string name;
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<std::pair<double, int>> arrivals; // (t, Lin)
    std::vector<int> Lout;
};

struct PolicyStats {
    std::string policyName;
    double meanScore = 0.0;
    double minScore = 1000.0;
    double maxScore = 0.0;
    double p5Score = 0.0;
    double meanTpScore = 0.0;
    double meanWaitingScore = 0.0;
    double meanTdr = 0.0;
    double meanTpot = 0.0;
    double worstTpot = 0.0;
    int protocolViolations = 0;
    int stuckSimulations = 0;
    double meanDpreBatch = 0.0;
    double meanDprocBatch = 0.0;
    double meanDpostBatch = 0.0;
};

struct SingleRunResult {
    bool success = false;
    double totalTime = 0.0;
    long long totalTokens = 0;
    double tp = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
    double excessTdr = 0.0;
    double excessTpot = 0.0;
    double dist = 0.0;
    double tpScore = 0.0;
    double waitingScore = 0.0;
    double finalScore = 0.0;
    int dpreGroups = 0;
    double dpreBatchSum = 0.0;
    int dprocGroups = 0;
    double dprocBatchSum = 0.0;
    int dpostGroups = 0;
    double dpostBatchSum = 0.0;
};

static double clampVal(double x, double baseVal, double targetVal) {
    if (targetVal == baseVal) return 0.0;
    double val = (x - baseVal) / (targetVal - baseVal);
    return std::max(0.0, std::min(1.0, val));
}

// In-process fast deterministic simulator for policy analysis
SingleRunResult simulatePolicy(const SimScenario& scenario, int policyId) {
    SingleRunResult res;
    
    int K = scenario.sys.K;
    double S = scenario.sys.S;
    int num_layers = scenario.sys.num_layers;

    // Pre-calculate m_SLO_star (max m where S + dur(m) <= SLO2)
    int m_SLO_star = 1;
    for (int m = 1; m <= 4096; ++m) {
        double dur = scenario.table.getDuration(TaskStep::DECODE_PROC, m);
        if (S + dur <= scenario.sc.SLO2) {
            m_SLO_star = m;
        } else {
            break;
        }
    }
    if (m_SLO_star < 1) m_SLO_star = 1;

    // Pre-calculate m_eff_star (max marginal efficiency m / (S + dur(m)))
    int m_eff_star = 1;
    double bestEff = 0.0;
    for (int m = 1; m <= 128; ++m) {
        double dur = scenario.table.getDuration(TaskStep::DECODE_PROC, m);
        double eff = static_cast<double>(m) / (S + dur);
        if (eff > bestEff) {
            bestEff = eff;
            m_eff_star = m;
        }
    }

    struct ReqState {
        int rid;
        double arrTime;
        int Lin;
        int Lout;
        int stage = 0; // 0:ARR, 1:P_PRE_IN, 2:P_WAIT_UP, 3:P_PROC_READY, 4:P_PROC_IN, 5:P_WAIT_DOWN, 6:P_POST_READY, 7:D_PRE_READY, 8:D_PRE_IN, 9:D_WAIT_UP, 10:D_PROC_READY, 11:D_PROC_IN, 12:D_WAIT_DOWN, 13:D_POST_READY, 14:FINISHED
        int assignedRemote = -1;
        int nextLayerStart = 0;
        int tokensDone = 0;
        double ppostTime = 0.0;
        std::vector<double> tokenTimes;
    };

    int R = scenario.arrivals.size();
    std::vector<ReqState> reqs(R);
    for (int i = 0; i < R; ++i) {
        reqs[i] = {i, scenario.arrivals[i].first, scenario.arrivals[i].second, scenario.Lout[i], 0, -1, 0, 0, 0.0, {}};
    }

    bool edgeBusy = false;
    std::vector<bool> cloudBusy(K, false);
    std::vector<double> cloudWorkload(K, 0.0);

    struct Event {
        double time;
        int type; // 0:ARR, 1:TDN_E, 2:TDN_C, 3:XDN_UP, 4:XDN_DOWN
        int server;
        int rid;
        int m;
        std::vector<int> rids;
        int nextStage;
    };

    auto comp = [](const Event& a, const Event& b) { return a.time > b.time; };
    std::vector<Event> eventQueue;

    auto pushEv = [&](double t_ev, int type, int server, int rid, int m, const std::vector<int>& rids, int nextStage) {
        eventQueue.push_back({t_ev, type, server, rid, m, rids, nextStage});
        std::push_heap(eventQueue.begin(), eventQueue.end(), comp);
    };

    for (int i = 0; i < R; ++i) {
        pushEv(scenario.arrivals[i].first, 0, -1, i, 1, {i}, 0);
    }

    double currentTime = 0.0;
    int finishedCount = 0;
    int simSteps = 0;

    while (!eventQueue.empty() && finishedCount < R && simSteps < 500000) {
        simSteps++;
        std::pop_heap(eventQueue.begin(), eventQueue.end(), comp);
        Event ev = eventQueue.back();
        eventQueue.pop_back();

        currentTime = ev.time;

        if (ev.type == 0) {
            reqs[ev.rid].stage = 0;
        } else if (ev.type == 1) {
            edgeBusy = false;
            if (ev.nextStage == 2) {
                reqs[ev.rid].stage = 2;
                long long bytes = reqs[ev.rid].Lin * scenario.sys.bytes_per_token;
                double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                pushEv(currentTime + trDur, 3, reqs[ev.rid].assignedRemote, ev.rid, 1, {ev.rid}, 3);
            } else if (ev.nextStage == 7) {
                reqs[ev.rid].stage = 7;
                reqs[ev.rid].ppostTime = currentTime;
            } else if (ev.nextStage == 9) {
                std::map<int, std::vector<int>> perRemote;
                for (int rid : ev.rids) {
                    reqs[rid].stage = 9;
                    perRemote[reqs[rid].assignedRemote].push_back(rid);
                }
                for (const auto& kv : perRemote) {
                    int sub_m = kv.second.size();
                    long long bytes = sub_m * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    pushEv(currentTime + trDur, 3, kv.first, kv.second[0], sub_m, kv.second, 10);
                }
            } else if (ev.nextStage == 14) {
                for (int rid : ev.rids) {
                    reqs[rid].tokensDone++;
                    reqs[rid].tokenTimes.push_back(currentTime);
                    if (reqs[rid].tokensDone == reqs[rid].Lout) {
                        reqs[rid].stage = 14;
                        finishedCount++;
                    } else {
                        reqs[rid].stage = 7;
                    }
                }
            }
        } else if (ev.type == 2) {
            cloudBusy[ev.server] = false;
            if (ev.nextStage == 5) {
                int rid = ev.rid;
                if (reqs[rid].nextLayerStart == num_layers) {
                    reqs[rid].stage = 5;
                    long long bytes = reqs[rid].Lin * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    pushEv(currentTime + trDur, 4, ev.server, rid, 1, {rid}, 6);
                } else {
                    reqs[rid].stage = 3;
                }
            } else if (ev.nextStage == 12) {
                for (int rid : ev.rids) reqs[rid].stage = 12;
                long long bytes = ev.m * scenario.sys.bytes_per_token;
                double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                pushEv(currentTime + trDur, 4, ev.server, ev.rids[0], ev.m, ev.rids, 13);
            }
        } else if (ev.type == 3) {
            for (int rid : ev.rids) reqs[rid].stage = ev.nextStage;
        } else if (ev.type == 4) {
            for (int rid : ev.rids) reqs[rid].stage = ev.nextStage;
        }

        while (!eventQueue.empty() && eventQueue.front().time == currentTime) {
            std::pop_heap(eventQueue.begin(), eventQueue.end(), comp);
            Event evC = eventQueue.back();
            eventQueue.pop_back();

            if (evC.type == 0) reqs[evC.rid].stage = 0;
            else if (evC.type == 1) {
                edgeBusy = false;
                if (evC.nextStage == 2) {
                    reqs[evC.rid].stage = 2;
                    long long bytes = reqs[evC.rid].Lin * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    pushEv(currentTime + trDur, 3, reqs[evC.rid].assignedRemote, evC.rid, 1, {evC.rid}, 3);
                } else if (evC.nextStage == 7) {
                    reqs[evC.rid].stage = 7;
                    reqs[evC.rid].ppostTime = currentTime;
                } else if (evC.nextStage == 9) {
                    std::map<int, std::vector<int>> perRemote;
                    for (int rid : evC.rids) {
                        reqs[rid].stage = 9;
                        perRemote[reqs[rid].assignedRemote].push_back(rid);
                    }
                    for (const auto& kv : perRemote) {
                        int sub_m = kv.second.size();
                        long long bytes = sub_m * scenario.sys.bytes_per_token;
                        double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                        pushEv(currentTime + trDur, 3, kv.first, kv.second[0], sub_m, kv.second, 10);
                    }
                } else if (evC.nextStage == 14) {
                    for (int rid : evC.rids) {
                        reqs[rid].tokensDone++;
                        reqs[rid].tokenTimes.push_back(currentTime);
                        if (reqs[rid].tokensDone == reqs[rid].Lout) {
                            reqs[rid].stage = 14;
                            finishedCount++;
                        } else {
                            reqs[rid].stage = 7;
                        }
                    }
                }
            } else if (evC.type == 2) {
                cloudBusy[evC.server] = false;
                if (evC.nextStage == 5) {
                    int rid = evC.rid;
                    if (reqs[rid].nextLayerStart == num_layers) {
                        reqs[rid].stage = 5;
                        long long bytes = reqs[rid].Lin * scenario.sys.bytes_per_token;
                        double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                        pushEv(currentTime + trDur, 4, evC.server, rid, 1, {rid}, 6);
                    } else {
                        reqs[rid].stage = 3;
                    }
                } else if (evC.nextStage == 12) {
                    for (int rid : evC.rids) reqs[rid].stage = 12;
                    long long bytes = evC.m * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    pushEv(currentTime + trDur, 4, evC.server, evC.rids[0], evC.m, evC.rids, 13);
                }
            } else if (evC.type == 3) {
                for (int rid : evC.rids) reqs[rid].stage = evC.nextStage;
            } else if (evC.type == 4) {
                for (int rid : evC.rids) reqs[rid].stage = evC.nextStage;
            }
        }

        std::vector<int> ppreReady, ppostReady, dpreReady, dpostReady;
        std::vector<std::vector<int>> pprocReady(K), dprocReady(K);

        int idleCloudCount = 0;
        for (int k = 0; k < K; ++k) {
            if (!cloudBusy[k]) idleCloudCount++;
        }

        for (int i = 0; i < R; ++i) {
            if (reqs[i].stage == 0) ppreReady.push_back(i);
            else if (reqs[i].stage == 3) pprocReady[reqs[i].assignedRemote].push_back(i);
            else if (reqs[i].stage == 6) ppostReady.push_back(i);
            else if (reqs[i].stage == 7) dpreReady.push_back(i);
            else if (reqs[i].stage == 10) dprocReady[reqs[i].assignedRemote].push_back(i);
            else if (reqs[i].stage == 13) dpostReady.push_back(i);
        }

        if (!edgeBusy) {
            if (!dpostReady.empty()) {
                int batchSize = dpostReady.size();
                if (policyId == 1) batchSize = 1;
                else if (policyId == 2 && (scenario.sc.dist_base == 0.0 || scenario.sc.w_c > 0.5)) batchSize = std::min<int>(dpostReady.size(), m_SLO_star);

                std::vector<int> batch(dpostReady.begin(), dpostReady.begin() + batchSize);
                for (int rid : batch) reqs[rid].stage = 14;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::DECODE_POST, batchSize);
                pushEv(currentTime + S + dur, 1, -1, batch[0], batchSize, batch, 14);

                res.dpostGroups++;
                res.dpostBatchSum += batchSize;

            } else if (!ppostReady.empty()) {
                int rid = ppostReady[0];
                reqs[rid].stage = 7;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::PREFILL_POST, reqs[rid].Lin);
                pushEv(currentTime + S + dur, 1, -1, rid, 1, {rid}, 7);

            } else if (!dpreReady.empty()) {
                int batchSize = dpreReady.size();
                
                if (policyId == 1) {
                    batchSize = 1;
                } else if (policyId == 2 && (scenario.sc.dist_base == 0.0 || scenario.sc.w_c > 0.5)) {
                    batchSize = std::min<int>(dpreReady.size(), m_SLO_star);
                } else if (policyId == 3 || policyId == 4) {
                    if (idleCloudCount > 0) {
                        batchSize = std::min<int>(dpreReady.size(), idleCloudCount);
                    } else if (scenario.sc.dist_base == 0.0 || scenario.sc.w_c > 0.3) {
                        batchSize = std::min<int>(dpreReady.size(), std::min(m_eff_star, m_SLO_star));
                    }
                }

                if (batchSize < 1) batchSize = 1;
                std::vector<int> batch(dpreReady.begin(), dpreReady.begin() + batchSize);
                for (int rid : batch) reqs[rid].stage = 8;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::DECODE_PRE, batchSize);
                pushEv(currentTime + S + dur, 1, -1, batch[0], batchSize, batch, 9);

                res.dpreGroups++;
                res.dpreBatchSum += batchSize;

            } else if (!ppreReady.empty()) {
                int rid = ppreReady[0];
                int bestCloud = 0;
                double minWork = cloudWorkload[0];
                for (int k = 1; k < K; ++k) {
                    if (cloudWorkload[k] < minWork) {
                        minWork = cloudWorkload[k];
                        bestCloud = k;
                    }
                }
                reqs[rid].assignedRemote = bestCloud;
                reqs[rid].stage = 1;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::PREFILL_PRE, reqs[rid].Lin);
                pushEv(currentTime + S + dur, 1, -1, rid, 1, {rid}, 2);
            }
        }

        for (int k = 0; k < K; ++k) {
            if (cloudBusy[k]) continue;

            if (!dprocReady[k].empty()) {
                int batchSize = dprocReady[k].size();
                if (policyId == 1) batchSize = 1;
                else if ((policyId == 2 || policyId == 4) && (scenario.sc.dist_base == 0.0 || scenario.sc.w_c > 0.3)) {
                    batchSize = std::min<int>(dprocReady[k].size(), m_SLO_star);
                }
                if (batchSize < 1) batchSize = 1;

                std::vector<int> batch(dprocReady[k].begin(), dprocReady[k].begin() + batchSize);
                for (int rid : batch) reqs[rid].stage = 11;
                cloudBusy[k] = true;
                double dur = scenario.table.getDuration(TaskStep::DECODE_PROC, batchSize);
                pushEv(currentTime + S + dur, 2, k, batch[0], batchSize, batch, 12);

                res.dprocGroups++;
                res.dprocBatchSum += batchSize;

            } else if (!pprocReady[k].empty()) {
                int rid = pprocReady[k][0];
                reqs[rid].stage = 4;
                reqs[rid].nextLayerStart = num_layers;
                cloudBusy[k] = true;
                double dur = scenario.table.getDuration(TaskStep::PREFILL_PROC, reqs[rid].Lin);
                pushEv(currentTime + S + dur, 2, k, rid, 1, {rid}, 5);
            }
        }
    }

    if (finishedCount == R) {
        res.success = true;

        double minArr = reqs[0].arrTime;
        double maxTokenTime = 0.0;
        long long totalTokens = 0;

        double sumTdr = 0.0;
        double sumTpotGaps = 0.0;
        long long tpotGapCount = 0;

        for (int i = 0; i < R; ++i) {
            const auto& r = reqs[i];
            totalTokens += r.Lout;
            minArr = std::min(minArr, r.arrTime);
            if (!r.tokenTimes.empty()) {
                maxTokenTime = std::max(maxTokenTime, r.tokenTimes.back());
            }
            sumTdr += (r.ppostTime - r.arrTime);

            for (size_t j = 0; j + 1 < r.tokenTimes.size(); ++j) {
                double gap = r.tokenTimes[j+1] - r.tokenTimes[j];
                sumTpotGaps += gap;
                tpotGapCount++;
            }
        }

        res.totalTime = maxTokenTime - minArr;
        res.totalTokens = totalTokens;
        res.tp = (res.totalTime > 0) ? (totalTokens / res.totalTime) : 0.0;
        res.tdr = sumTdr / R;
        res.tpot = (tpotGapCount > 0) ? (sumTpotGaps / tpotGapCount) : 0.0;

        res.tpScore = 1000.0 * scenario.sc.w_tp * clampVal(res.tp, scenario.sc.tp_base, scenario.sc.tp_UB);

        res.excessTdr = std::max(0.0, (res.tdr - scenario.sc.SLO1) / scenario.sc.SLO1);
        res.excessTpot = std::max(0.0, (res.tpot - scenario.sc.SLO2) / scenario.sc.SLO2);
        res.dist = std::sqrt(res.excessTdr * res.excessTdr + res.excessTpot * res.excessTpot);

        double waitingComp = 0.0;
        if (scenario.sc.dist_base > 0) {
            waitingComp = clampVal(res.dist, scenario.sc.dist_base, 0.0);
        } else {
            waitingComp = (res.dist == 0.0) ? 1.0 : 0.0;
        }
        res.waitingScore = 1000.0 * scenario.sc.w_c * waitingComp;

        res.finalScore = res.tpScore + res.waitingScore;
    }

    return res;
}

std::vector<SimScenario> generate5000ScenarioSuite() {
    std::vector<SimScenario> scenarios;
    int idCounter = 1;

    std::vector<int> K_list = {1, 2, 4, 8};
    std::vector<int> R_list = {2, 5, 10, 25, 50, 100};
    
    enum class ArrPattern { SIMULTANEOUS, EVEN, BURST, SPARSE, MIXED };
    std::vector<ArrPattern> patterns = { ArrPattern::SIMULTANEOUS, ArrPattern::EVEN, ArrPattern::BURST, ArrPattern::SPARSE, ArrPattern::MIXED };

    struct Profile { double w_tp; double w_c; std::string label; };
    std::vector<Profile> profiles = {
        {1.0, 0.0, "ProfA_TP1.0_WC0.0"},
        {0.9, 0.1, "ProfB_TP0.9_WC0.1"},
        {0.5, 0.5, "ProfC_TP0.5_WC0.5"},
        {0.1, 0.9, "ProfD_TP0.1_WC0.9"},
        {0.0, 1.0, "ProfE_TP0.0_WC1.0"}
    };

    std::vector<double> dist_bases = {2.0, 0.5, 0.0};
    std::vector<double> SLO2_list = {5.0, 15.0, 50.0};

    for (int K : K_list) {
        for (int R : R_list) {
            for (auto pat : patterns) {
                for (const auto& prof : profiles) {
                    for (double db : dist_bases) {
                        for (double slo2 : SLO2_list) {
                            SimScenario sc;
                            sc.id = idCounter++;
                            sc.name = "Scen#" + std::to_string(sc.id) + " K=" + std::to_string(K) + " R=" + std::to_string(R) + " " + prof.label + " SLO2=" + std::to_string((int)slo2) + " db=" + std::to_string(db);

                            sc.sys.K = K;
                            sc.sys.S = 1.0;
                            sc.sys.num_layers = 4;
                            sc.sys.latency_in_ms = 1.0;
                            sc.sys.bandwidth_gbps = 2.0;
                            sc.sys.bytes_per_token = 125000;

                            sc.sc.w_tp = prof.w_tp;
                            sc.sc.w_c = prof.w_c;
                            sc.sc.dist_base = db;
                            sc.sc.SLO1 = 50.0;
                            sc.sc.SLO2 = slo2;
                            sc.sc.tp_base = 0.01;
                            sc.sc.tp_UB = 0.50;

                            sc.table.N = 4;
                            sc.table.raw_rows = {
                                {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
                                {4, 3.0, 10.0, 2.0, 1.5, 6.0, 1.5},
                                {16, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0},
                                {100, 3.0, 10.0, 2.0, 4.0, 16.0, 4.0}
                            };

                            for (int i = 0; i < R; ++i) {
                                double t_arr = 0.0;
                                if (pat == ArrPattern::SIMULTANEOUS) t_arr = 0.0;
                                else if (pat == ArrPattern::EVEN) t_arr = i * 2.0;
                                else if (pat == ArrPattern::BURST) t_arr = (i / 5) * 50.0;
                                else if (pat == ArrPattern::SPARSE) t_arr = i * 40.0;
                                else if (pat == ArrPattern::MIXED) t_arr = (i < R / 2) ? (i * 1.0) : (100.0 + (i - R / 2) * 30.0);

                                sc.arrivals.push_back({t_arr, 256});
                                sc.Lout.push_back(16);
                            }

                            scenarios.push_back(sc);
                            if (scenarios.size() >= 5400) break;
                        }
                        if (scenarios.size() >= 5400) break;
                    }
                    if (scenarios.size() >= 5400) break;
                }
                if (scenarios.size() >= 5400) break;
            }
            if (scenarios.size() >= 5400) break;
        }
        if (scenarios.size() >= 5400) break;
    }

    return scenarios;
}

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     COUNTEREXAMPLE-DRIVEN POLICY ANALYSIS BENCHMARK (5,400 SCENARIOS)                    \n";
    std::cout << "=========================================================================================\n";

    auto suite = generate5000ScenarioSuite();
    std::cout << "Successfully generated " << suite.size() << " test scenarios.\n\n";

    std::vector<std::string> policyNames = {
        "Policy 0: V2 Greedy (Baseline)",
        "Policy 1: V1 Reference (m=1)",
        "Policy 2: Naive SLO2-Cap (Draft Rules 1&3)",
        "Policy 3: Cloud-Aware Idle-Capacity Policy",
        "Policy 4: Marginal Efficiency + Latency Budget (RECOMMENDED)"
    };

    std::vector<PolicyStats> stats(5);
    for (int p = 0; p < 5; ++p) stats[p].policyName = policyNames[p];

    std::vector<std::vector<double>> allScores(5);

    struct Regression {
        SimScenario sc;
        double v2Score;
        double pol4Score;
        double diff;
    };
    std::vector<Regression> v2Regressions;

    for (size_t i = 0; i < suite.size(); ++i) {
        const auto& sc = suite[i];

        for (int p = 0; p < 5; ++p) {
            SingleRunResult r = simulatePolicy(sc, p);
            if (!r.success) {
                stats[p].stuckSimulations++;
                continue;
            }

            stats[p].meanScore += r.finalScore;
            stats[p].minScore = std::min(stats[p].minScore, r.finalScore);
            stats[p].maxScore = std::max(stats[p].maxScore, r.finalScore);
            stats[p].meanTpScore += r.tpScore;
            stats[p].meanWaitingScore += r.waitingScore;
            stats[p].meanTdr += r.tdr;
            stats[p].meanTpot += r.tpot;
            stats[p].worstTpot = std::max(stats[p].worstTpot, r.tpot);

            if (r.dpreGroups > 0) stats[p].meanDpreBatch += (r.dpreBatchSum / r.dpreGroups);
            if (r.dprocGroups > 0) stats[p].meanDprocBatch += (r.dprocBatchSum / r.dprocGroups);
            if (r.dpostGroups > 0) stats[p].meanDpostBatch += (r.dpostBatchSum / r.dpostGroups);

            allScores[p].push_back(r.finalScore);
        }

        SingleRunResult r2 = simulatePolicy(sc, 0); // V2
        SingleRunResult r4 = simulatePolicy(sc, 4); // Recommended Policy 4
        if (r2.success && r4.success && r2.finalScore > r4.finalScore + 1e-3) {
            v2Regressions.push_back({sc, r2.finalScore, r4.finalScore, r2.finalScore - r4.finalScore});
        }
    }

    size_t N = suite.size();
    for (int p = 0; p < 5; ++p) {
        stats[p].meanScore /= N;
        stats[p].meanTpScore /= N;
        stats[p].meanWaitingScore /= N;
        stats[p].meanTdr /= N;
        stats[p].meanTpot /= N;
        stats[p].meanDpreBatch /= N;
        stats[p].meanDprocBatch /= N;
        stats[p].meanDpostBatch /= N;

        std::sort(allScores[p].begin(), allScores[p].end());
        size_t p5Idx = static_cast<size_t>(N * 0.05);
        stats[p].p5Score = allScores[p][p5Idx];
    }

    std::cout << "=========================================================================================\n";
    std::cout << "COMPREHENSIVE POLICY COMPARISON MATRIX (AVERAGED OVER 5,400 SCENARIOS)\n";
    std::cout << "=========================================================================================\n";
    std::cout << std::left << std::setw(30) << "Policy Name" 
              << std::setw(12) << "MeanScore" 
              << std::setw(12) << "MinScore" 
              << std::setw(12) << "5th %ile" 
              << std::setw(12) << "TpScore" 
              << std::setw(12) << "WaitScore" 
              << std::setw(12) << "Mean TPOT" 
              << "Worst TPOT\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";

    for (int p = 0; p < 5; ++p) {
        std::cout << std::left << std::setw(30) << stats[p].policyName.substr(0, 28)
                  << std::setw(12) << std::fixed << std::setprecision(1) << stats[p].meanScore
                  << std::setw(12) << stats[p].minScore
                  << std::setw(12) << stats[p].p5Score
                  << std::setw(12) << stats[p].meanTpScore
                  << std::setw(12) << stats[p].meanWaitingScore
                  << std::setw(12) << stats[p].meanTpot
                  << stats[p].worstTpot << "\n";
    }

    std::cout << "\n=========================================================================================\n";
    std::cout << "TOP 10 WORST REGRESSIONS OF RECOMMENDED POLICY 4 VS CURRENT V2 GREEDY\n";
    std::cout << "=========================================================================================\n";
    std::sort(v2Regressions.begin(), v2Regressions.end(), [](const Regression& a, const Regression& b) {
        return a.diff > b.diff;
    });

    std::cout << std::left << std::setw(8) << "ScenID" << std::setw(40) << "Scenario Name" << std::setw(14) << "V2 Score" << std::setw(14) << "Policy4 Score" << "Regression\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";
    for (size_t i = 0; i < std::min<size_t>(10, v2Regressions.size()); ++i) {
        const auto& r = v2Regressions[i];
        std::cout << std::left << std::setw(8) << r.sc.id << std::setw(40) << r.sc.name.substr(0, 38) << std::setw(14) << r.v2Score << std::setw(14) << r.pol4Score << "-" << r.diff << "\n";
    }

    std::cout << "\nTotal Regressions vs V2 out of 5,400 scenarios: " << v2Regressions.size() << " (" << (100.0 * v2Regressions.size() / N) << "%)\n";

    return 0;
}
