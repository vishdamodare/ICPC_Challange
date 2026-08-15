#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <iomanip>
#include <string>
#include <algorithm>
#include <random>
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
    std::string name;
    double meanScore = 0.0;
    double medianScore = 0.0;
    double minScore = 1000.0;
    double p1Score = 0.0;
    double p5Score = 0.0;
    double p10Score = 0.0;
    double maxScore = 0.0;
    double meanTpScore = 0.0;
    double meanWaitingScore = 0.0;
    double meanTdr = 0.0;
    double meanTpot = 0.0;
    double worstTpot = 0.0;
    double meanTotalTime = 0.0;
    
    int improved = 0;
    int unchanged = 0;
    int regressed = 0;
    
    double worstRegression = 0.0;
    double worstImprovement = 0.0;
    
    double meanDpreBatch = 0.0;
    double totalDpreTasks = 0.0;
    double totalCloudIdleTime = 0.0;
};

struct RunResult {
    bool success = false;
    double totalTime = 0.0;
    long long totalTokens = 0;
    double tp = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
    double worstTpot = 0.0;
    double excessTdr = 0.0;
    double excessTpot = 0.0;
    double dist = 0.0;
    double tpScore = 0.0;
    double waitingScore = 0.0;
    double finalScore = 0.0;
    
    int dpreGroups = 0;
    double dpreBatchSum = 0.0;
    double cloudIdleTimeSum = 0.0;
};

static double clampVal(double x, double baseVal, double targetVal) {
    if (targetVal == baseVal) return 0.0;
    double val = (x - baseVal) / (targetVal - baseVal);
    return std::max(0.0, std::min(1.0, val));
}

RunResult simulateScenario(const SimScenario& scenario, int policyId) {
    RunResult res;

    int K = scenario.sys.K;
    double S = scenario.sys.S;
    int num_layers = scenario.sys.num_layers;

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

    double lastCloudStateCheck = 0.0;

    while (!eventQueue.empty() && finishedCount < R && simSteps < 500000) {
        simSteps++;
        std::pop_heap(eventQueue.begin(), eventQueue.end(), comp);
        Event ev = eventQueue.back();
        eventQueue.pop_back();

        double dt = ev.time - currentTime;
        currentTime = ev.time;

        int idleCloudsNow = 0;
        for (int k = 0; k < K; ++k) {
            if (!cloudBusy[k]) idleCloudsNow++;
        }
        res.cloudIdleTimeSum += (idleCloudsNow * dt);

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
        int totalInFlightClouds = 0;
        for (int k = 0; k < K; ++k) {
            if (!cloudBusy[k]) idleCloudCount++;
            else totalInFlightClouds++;
        }

        for (int i = 0; i < R; ++i) {
            if (reqs[i].stage == 0) ppreReady.push_back(i);
            else if (reqs[i].stage == 3) pprocReady[reqs[i].assignedRemote].push_back(i);
            else if (reqs[i].stage == 6) ppostReady.push_back(i);
            else if (reqs[i].stage == 7) dpreReady.push_back(i);
            else if (reqs[i].stage == 10) dprocReady[reqs[i].assignedRemote].push_back(i);
            else if (reqs[i].stage == 13) dpostReady.push_back(i);
        }

        // Compute oldest ready age
        double oldestAge = 0.0;
        if (!dpreReady.empty()) {
            double minArr = currentTime;
            for (int rid : dpreReady) minArr = std::min(minArr, reqs[rid].arrTime);
            oldestAge = currentTime - minArr;
        }

        if (!edgeBusy) {
            if (!dpostReady.empty()) { // D_POST (All ready)
                int batchSize = dpostReady.size();
                std::vector<int> batch(dpostReady.begin(), dpostReady.begin() + batchSize);
                for (int rid : batch) reqs[rid].stage = 14;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::DECODE_POST, batchSize);
                pushEv(currentTime + S + dur, 1, -1, batch[0], batchSize, batch, 14);

            } else if (!ppostReady.empty()) { // P_POST
                int rid = ppostReady[0];
                reqs[rid].stage = 7;
                edgeBusy = true;
                double dur = scenario.table.getDuration(TaskStep::PREFILL_POST, reqs[rid].Lin);
                pushEv(currentTime + S + dur, 1, -1, rid, 1, {rid}, 7);

            } else if (!dpreReady.empty()) { // D_PRE DISPATCH LOGIC ACROSS POLICIES
                int batchSize = dpreReady.size();
                int readyN = dpreReady.size();

                if (policyId == 0) { // P0: Baseline V2 Max Batching
                    batchSize = readyN;

                } else if (policyId == 1) { // P1: Naive idleClouds
                    if (idleCloudCount > 0) batchSize = std::min(readyN, idleCloudCount);

                } else if (policyId == 2) { // P2: Marginal Batch-Efficiency Threshold
                    double costMax = S + scenario.table.getDuration(TaskStep::DECODE_PRE, readyN);
                    double effMax = static_cast<double>(readyN) / costMax;

                    int subM = std::min(readyN, std::max(1, idleCloudCount));
                    double costSub = S + scenario.table.getDuration(TaskStep::DECODE_PRE, subM);
                    double effSub = static_cast<double>(subM) / costSub;

                    // If efficiency drop is small (< 15%), allow splitting to feed idle clouds
                    if (idleCloudCount > 0 && (effSub >= 0.85 * effMax)) {
                        batchSize = subM;
                    } else {
                        batchSize = readyN;
                    }

                } else if (policyId == 3) { // P3: Oldest-Ready Age Threshold
                    if (oldestAge > S * 2.0 && idleCloudCount > 0) {
                        batchSize = std::min(readyN, idleCloudCount);
                    } else {
                        batchSize = readyN;
                    }

                } else if (policyId == 4) { // P4: Combined Efficiency + Age
                    if (idleCloudCount > 0 && (readyN >= K || oldestAge > 10.0)) {
                        batchSize = std::min(readyN, idleCloudCount);
                    } else {
                        batchSize = readyN;
                    }

                } else if (policyId == 5) { // P5: HYBRID Combined Efficiency + Cloud-Starvation
                    // Starvation check: clouds sit COMPLETELY idle with no in-flight work
                    bool cloudStarving = (idleCloudCount == K) || (totalInFlightClouds == 0 && idleCloudCount > 0);
                    if (cloudStarving || readyN >= K || oldestAge > 15.0) {
                        batchSize = std::min(readyN, std::max(1, idleCloudCount));
                    } else {
                        batchSize = readyN;
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

            } else if (!ppreReady.empty()) { // P_PRE
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
                std::vector<int> batch(dprocReady[k].begin(), dprocReady[k].begin() + batchSize);
                for (int rid : batch) reqs[rid].stage = 11;
                cloudBusy[k] = true;
                double dur = scenario.table.getDuration(TaskStep::DECODE_PROC, batchSize);
                pushEv(currentTime + S + dur, 2, k, batch[0], batchSize, batch, 12);

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
                res.worstTpot = std::max(res.worstTpot, gap);
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

// 20,000 Scenario Generator (Fixed Seed 42)
std::vector<SimScenario> generate20000ScenarioSuite() {
    std::vector<SimScenario> scenarios;
    scenarios.reserve(20000);
    std::mt19937 rng(42);

    int idCounter = 1;

    for (int i = 0; i < 20000; ++i) {
        SimScenario sc;
        sc.id = idCounter++;

        int K_choice = rng() % 10;
        int K = (K_choice < 4) ? 4 : ((K_choice < 8) ? 8 : ((K_choice == 8) ? 2 : 1));
        sc.sys.K = K;

        int R_choice = rng() % 10;
        int R = 10;
        if (R_choice == 0) R = 2;
        else if (R_choice == 1) R = 5;
        else if (R_choice <= 4) R = 10;
        else if (R_choice <= 6) R = 16;
        else if (R_choice == 7) R = 25;
        else if (R_choice == 8) R = 50;
        else R = 100;

        int pat = rng() % 5;
        std::string patName = (pat == 0 ? "Simult" : (pat == 1 ? "Even" : (pat == 2 ? "Burst" : (pat == 3 ? "Sparse" : "Mixed"))));

        int profChoice = rng() % 5;
        double w_tp = (profChoice == 0) ? 1.0 : ((profChoice == 1) ? 0.9 : ((profChoice == 2) ? 0.5 : ((profChoice == 3) ? 0.1 : 0.0)));
        double w_c = 1.0 - w_tp;

        int dbChoice = rng() % 3;
        double db = (dbChoice == 0) ? 0.0 : ((dbChoice == 1) ? 0.5 : 2.0);

        int sloChoice = rng() % 3;
        double slo2 = (sloChoice == 0) ? 5.0 : ((sloChoice == 1) ? 15.0 : 40.0);

        sc.sys.S = (rng() % 5) + 1.0;
        sc.sys.num_layers = 4;
        sc.sys.latency_in_ms = 1.0;
        sc.sys.bandwidth_gbps = 2.0;
        sc.sys.bytes_per_token = 125000;

        sc.sc.w_tp = w_tp;
        sc.sc.w_c = w_c;
        sc.sc.dist_base = db;
        sc.sc.SLO1 = 40.0;
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

        for (int r = 0; r < R; ++r) {
            double t_arr = 0.0;
            if (pat == 0) t_arr = 0.0;
            else if (pat == 1) t_arr = r * 2.0;
            else if (pat == 2) t_arr = (r / 5) * 50.0;
            else if (pat == 3) t_arr = r * 40.0;
            else if (pat == 4) t_arr = (r < R / 2) ? (r * 1.0) : (100.0 + (r - R / 2) * 30.0);

            int Lin = (rng() % 2 == 0) ? 128 : 512;
            int lout = (rng() % 2 == 0) ? 8 : 32;
            sc.arrivals.push_back({t_arr, Lin});
            sc.Lout.push_back(lout);
        }

        sc.name = "Scen#" + std::to_string(sc.id) + " K=" + std::to_string(K) + " R=" + std::to_string(R) + " " + patName + " w_tp=" + std::to_string(w_tp).substr(0,3) + " db=" + std::to_string(db).substr(0,3) + " SLO2=" + std::to_string((int)slo2);
        scenarios.push_back(sc);
    }

    return scenarios;
}

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     FINAL HYBRID POLICY SEARCH BENCHMARK (20,000 DETERMINISTIC SCENARIOS)                \n";
    std::cout << "=========================================================================================\n";

    auto suite = generate20000ScenarioSuite();
    std::cout << "Generated " << suite.size() << " deterministic scenarios with fixed seed 42.\n\n";

    std::vector<std::string> polNames = {
        "P0: Baseline V2 Greedy",
        "P1: Naive idleClouds",
        "P2: Batch-Efficiency Thresh",
        "P3: Oldest-Age Thresh",
        "P4: Combined Eff + Age",
        "P5: HYBRID Eff + Starvation"
    };

    std::vector<PolicyStats> stats(6);
    for (int p = 0; p < 6; ++p) stats[p].name = polNames[p];

    std::vector<std::vector<double>> allScores(6);
    for (int p = 0; p < 6; ++p) allScores[p].reserve(suite.size());

    struct RegRecord {
        SimScenario sc;
        double v2Score;
        double polScore;
        double delta;
    };
    std::vector<std::vector<RegRecord>> policyRegressions(6);

    for (size_t i = 0; i < suite.size(); ++i) {
        const auto& sc = suite[i];

        RunResult r0 = simulateScenario(sc, 0); // V2

        for (int p = 0; p < 6; ++p) {
            RunResult r = (p == 0) ? r0 : simulateScenario(sc, p);
            if (!r.success) continue;

            stats[p].meanScore += r.finalScore;
            stats[p].minScore = std::min(stats[p].minScore, r.finalScore);
            stats[p].maxScore = std::max(stats[p].maxScore, r.finalScore);
            stats[p].meanTpScore += r.tpScore;
            stats[p].meanWaitingScore += r.waitingScore;
            stats[p].meanTdr += r.tdr;
            stats[p].meanTpot += r.tpot;
            stats[p].worstTpot = std::max(stats[p].worstTpot, r.worstTpot);
            stats[p].meanTotalTime += r.totalTime;

            if (r.dpreGroups > 0) stats[p].meanDpreBatch += (r.dpreBatchSum / r.dpreGroups);
            stats[p].totalDpreTasks += r.dpreGroups;
            stats[p].totalCloudIdleTime += r.cloudIdleTimeSum;

            allScores[p].push_back(r.finalScore);

            if (p > 0) {
                double delta = r.finalScore - r0.finalScore;
                if (delta > 1e-3) {
                    stats[p].improved++;
                    stats[p].worstImprovement = std::max(stats[p].worstImprovement, delta);
                } else if (delta < -1e-3) {
                    stats[p].regressed++;
                    stats[p].worstRegression = std::min(stats[p].worstRegression, delta);
                    policyRegressions[p].push_back({sc, r0.finalScore, r.finalScore, delta});
                } else {
                    stats[p].unchanged++;
                }
            }
        }
    }

    size_t N = suite.size();
    for (int p = 0; p < 6; ++p) {
        stats[p].meanScore /= N;
        stats[p].meanTpScore /= N;
        stats[p].meanWaitingScore /= N;
        stats[p].meanTdr /= N;
        stats[p].meanTpot /= N;
        stats[p].meanTotalTime /= N;
        stats[p].meanDpreBatch /= N;
        stats[p].totalDpreTasks /= N;
        stats[p].totalCloudIdleTime /= N;

        std::sort(allScores[p].begin(), allScores[p].end());
        stats[p].medianScore = allScores[p][N / 2];
        stats[p].p1Score = allScores[p][static_cast<size_t>(N * 0.01)];
        stats[p].p5Score = allScores[p][static_cast<size_t>(N * 0.05)];
        stats[p].p10Score = allScores[p][static_cast<size_t>(N * 0.10)];
    }

    std::cout << "=========================================================================================\n";
    std::cout << "COMPREHENSIVE STATISTICAL COMPARISON MATRIX (20,000 SCENARIOS)\n";
    std::cout << "=========================================================================================\n";
    std::cout << std::left << std::setw(28) << "Policy Name" 
              << std::setw(11) << "Mean" 
              << std::setw(11) << "Median" 
              << std::setw(11) << "5th %ile" 
              << std::setw(11) << "Improved" 
              << std::setw(11) << "Regressed" 
              << std::setw(14) << "Worst Regr" 
              << "Mean TPOT\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";

    for (int p = 0; p < 6; ++p) {
        std::cout << std::left << std::setw(28) << stats[p].name.substr(0, 26)
                  << std::setw(11) << std::fixed << std::setprecision(1) << stats[p].meanScore
                  << std::setw(11) << stats[p].medianScore
                  << std::setw(11) << stats[p].p5Score
                  << std::setw(11) << (p == 0 ? 0 : stats[p].improved)
                  << std::setw(11) << (p == 0 ? 0 : stats[p].regressed)
                  << std::setw(14) << (p == 0 ? 0.0 : stats[p].worstRegression)
                  << stats[p].meanTpot << "\n";
    }

    std::cout << "\n=========================================================================================\n";
    std::cout << "TOP 20 WORST REGRESSIONS OF P5 HYBRID VS BASELINE V2 GREEDY\n";
    std::cout << "=========================================================================================\n";
    std::sort(policyRegressions[5].begin(), policyRegressions[5].end(), [](const RegRecord& a, const RegRecord& b) {
        return a.delta < b.delta;
    });

    std::cout << std::left << std::setw(8) << "ScenID" << std::setw(42) << "Scenario Name" << std::setw(12) << "V2 Score" << std::setw(12) << "P5 Score" << std::setw(12) << "ScoreDelta" << "Primary Cause\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";

    for (size_t i = 0; i < std::min<size_t>(20, policyRegressions[5].size()); ++i) {
        const auto& r = policyRegressions[5][i];
        std::cout << std::left << std::setw(8) << r.sc.id 
                  << std::setw(42) << r.sc.name.substr(0, 40) 
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.v2Score 
                  << std::setw(12) << r.polScore 
                  << std::setw(12) << r.delta 
                  << "D_PRE Suboptimal Dispatch\n";
    }

    return 0;
}
