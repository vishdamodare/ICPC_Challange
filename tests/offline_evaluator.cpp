#include "scenario_generator.hpp"
#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/reference_strategy.hpp"
#include "../src/greedy_strategy.hpp"
#include "../src/adaptive_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <map>

struct RunResult {
    bool success = false;
    double totalTime = 0.0;
    long long totalTokens = 0;
    double tp = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
    double dist = 0.0;
    double tpScore = 0.0;
    double waitingScore = 0.0;
    double finalScore = 0.0;
    long violations = 0;
};

static double clampVal(double x, double baseVal, double targetVal) {
    if (targetVal == baseVal) return 0.0;
    double val = (x - baseVal) / (targetVal - baseVal);
    return std::max(0.0, std::min(1.0, val));
}

RunResult simulateScenario(const Scenario& sc, SchedulingStrategy* strat) {
    RunResult res;

    int K = sc.sys.K;
    double S = sc.sys.S;
    int num_layers = sc.sys.num_layers;

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

    int R = sc.requests.size();
    std::vector<ReqState> reqs(R);
    for (int i = 0; i < R; ++i) {
        reqs[i] = {sc.requests[i].rid, sc.requests[i].arrivalTime, sc.requests[i].Lin, sc.requests[i].Lout, 0, -1, 0, 0, 0.0, {}};
    }

    bool edgeBusy = false;
    std::vector<bool> cloudBusy(K, false);

    struct SimEvent {
        double time;
        int type; // 0:ARR, 1:TDN_E, 2:TDN_C, 3:XDN_UP, 4:XDN_DOWN
        int server;
        int rid;
        int m;
        std::vector<int> rids;
        int nextStage;
        std::string taskSpec;
    };

    auto comp = [](const SimEvent& a, const SimEvent& b) { return a.time > b.time; };
    std::vector<SimEvent> eventQueue;

    auto pushEv = [&](double t_ev, int type, int server, int rid, int m, const std::vector<int>& rids, int nextStage, const std::string& spec = "") {
        eventQueue.push_back({t_ev, type, server, rid, m, rids, nextStage, spec});
        std::push_heap(eventQueue.begin(), eventQueue.end(), comp);
    };

    for (int i = 0; i < R; ++i) {
        pushEv(sc.requests[i].arrivalTime, 0, -1, i, 1, {i}, 0);
    }

    double currentTime = 0.0;
    int finishedCount = 0;
    int simSteps = 0;

    StateTracker state;
    state.init(sc.sys);

    while (!eventQueue.empty() && finishedCount < R && simSteps < 500000) {
        simSteps++;
        std::pop_heap(eventQueue.begin(), eventQueue.end(), comp);
        SimEvent ev = eventQueue.back();
        eventQueue.pop_back();

        currentTime = ev.time;

        if (ev.type == 0) {
            reqs[ev.rid].stage = 0;
        } else if (ev.type == 1) {
            edgeBusy = false;
            if (ev.nextStage == 2) {
                reqs[ev.rid].stage = 2;
                long long bytes = reqs[ev.rid].Lin * sc.sys.bytes_per_token;
                double trDur = sc.sys.latency_in_ms + 8.0 * bytes / (sc.sys.bandwidth_gbps * 1e6);
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
                    long long bytes = sub_m * sc.sys.bytes_per_token;
                    double trDur = sc.sys.latency_in_ms + 8.0 * bytes / (sc.sys.bandwidth_gbps * 1e6);
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
                    long long bytes = reqs[rid].Lin * sc.sys.bytes_per_token;
                    double trDur = sc.sys.latency_in_ms + 8.0 * bytes / (sc.sys.bandwidth_gbps * 1e6);
                    pushEv(currentTime + trDur, 4, ev.server, rid, 1, {rid}, 6);
                } else {
                    reqs[rid].stage = 3;
                }
            } else if (ev.nextStage == 12) {
                for (int rid : ev.rids) reqs[rid].stage = 12;
                long long bytes = ev.m * sc.sys.bytes_per_token;
                double trDur = sc.sys.latency_in_ms + 8.0 * bytes / (sc.sys.bandwidth_gbps * 1e6);
                pushEv(currentTime + trDur, 4, ev.server, ev.rids[0], ev.m, ev.rids, 13);
            }
        } else if (ev.type == 3) {
            for (int rid : ev.rids) reqs[rid].stage = ev.nextStage;
        } else if (ev.type == 4) {
            for (int rid : ev.rids) reqs[rid].stage = ev.nextStage;
        }

        // Build FrameContext for StateTracker
        FrameContext frame;
        frame.timestamp = currentTime;
        frame.eventCount = 1;
        
        ::Event evStruct;
        if (ev.type == 0) {
            evStruct.type = EventType::ARR;
            evStruct.rid = ev.rid;
            evStruct.Lin = reqs[ev.rid].Lin;
        } else if (ev.type == 1) {
            evStruct.type = EventType::TDN;
            evStruct.server = "E";
            evStruct.task_spec = ev.taskSpec;
        } else if (ev.type == 2) {
            evStruct.type = EventType::TDN;
            evStruct.server = "C" + std::to_string(ev.server);
            evStruct.task_spec = ev.taskSpec;
        } else if (ev.type == 3) {
            evStruct.type = EventType::XDN;
            evStruct.direction = "UP";
            evStruct.stage_tag = (ev.m > 1) ? "DEC" : "PRE";
            evStruct.rids = ev.rids;
        } else if (ev.type == 4) {
            evStruct.type = EventType::XDN;
            evStruct.direction = "DOWN";
            evStruct.stage_tag = (ev.m > 1) ? "DEC" : "PRE";
            evStruct.rids = ev.rids;
        }
        frame.events.push_back(evStruct);

        state.processFrame(frame);
        auto candidates = LegalTaskGenerator::generateCandidates(state);
        auto selected = strat->selectTasks(state, candidates);
        auto validTasks = ConflictResolver::resolveConflicts(state, selected);

        for (const auto& task : validTasks) {
            state.markTaskAssigned(task);

            if (task.server == -1) edgeBusy = true;
            else cloudBusy[task.server] = true;

            switch (task.type) {
                case TaskType::P_PRE: {
                    reqs[task.requests[0]].stage = 1;
                    reqs[task.requests[0]].assignedRemote = task.remote;
                    double dur = sc.table.getDuration(TaskStep::PREFILL_PRE, 1);
                    std::string spec = "P PRE " + std::to_string(task.remote) + " " + std::to_string(task.requests[0]);
                    pushEv(currentTime + dur, 1, -1, task.requests[0], 1, {task.requests[0]}, 2, spec);
                    break;
                }
                case TaskType::P_PROC: {
                    reqs[task.requests[0]].stage = 4;
                    reqs[task.requests[0]].nextLayerStart = task.le;
                    int layers = task.le - task.ls;
                    double base_dur = sc.table.getDuration(TaskStep::PREFILL_PROC, 1);
                    double dur = base_dur * layers / sc.sys.num_layers;
                    std::string spec = "P PROC " + std::to_string(task.ls) + " " + std::to_string(task.le) + " " + std::to_string(task.remote) + " " + std::to_string(task.requests[0]);
                    pushEv(currentTime + dur, 2, task.server, task.requests[0], 1, {task.requests[0]}, 5, spec);
                    break;
                }
                case TaskType::P_POST: {
                    reqs[task.requests[0]].stage = 6;
                    double dur = sc.table.getDuration(TaskStep::PREFILL_POST, 1);
                    std::string spec = "P POST " + std::to_string(task.remote) + " " + std::to_string(task.requests[0]);
                    pushEv(currentTime + dur, 1, -1, task.requests[0], 1, {task.requests[0]}, 7, spec);
                    break;
                }
                case TaskType::D_PRE: {
                    for (int rid : task.requests) reqs[rid].stage = 8;
                    double dur = sc.table.getDuration(TaskStep::DECODE_PRE, task.m);
                    std::string spec = "D PRE -1 " + std::to_string(task.m);
                    for (int r : task.requests) spec += " " + std::to_string(r);
                    pushEv(currentTime + dur, 1, -1, task.requests[0], task.m, task.requests, 9, spec);
                    break;
                }
                case TaskType::D_PROC: {
                    for (int rid : task.requests) reqs[rid].stage = 11;
                    double dur = sc.table.getDuration(TaskStep::DECODE_PROC, task.m);
                    std::string spec = "D PROC " + std::to_string(task.remote) + " " + std::to_string(task.m);
                    for (int r : task.requests) spec += " " + std::to_string(r);
                    pushEv(currentTime + dur, 2, task.server, task.requests[0], task.m, task.requests, 12, spec);
                    break;
                }
                case TaskType::D_POST: {
                    for (int rid : task.requests) reqs[rid].stage = 13;
                    double dur = sc.table.getDuration(TaskStep::DECODE_POST, task.m);
                    std::string spec = "D POST -1 " + std::to_string(task.m);
                    for (int r : task.requests) spec += " " + std::to_string(r);
                    pushEv(currentTime + dur, 1, -1, task.requests[0], task.m, task.requests, 14, spec);
                    break;
                }
            }
        }
    }

    res.success = (finishedCount == R);
    res.totalTime = currentTime;

    long long totalTokens = 0;
    std::vector<double> tdrVec;
    std::vector<double> tpotGaps;

    for (int i = 0; i < R; ++i) {
        totalTokens += reqs[i].tokensDone;
        if (reqs[i].ppostTime > reqs[i].arrTime) {
            tdrVec.push_back(reqs[i].ppostTime - reqs[i].arrTime);
        }
        for (size_t g = 1; g < reqs[i].tokenTimes.size(); ++g) {
            tpotGaps.push_back(reqs[i].tokenTimes[g] - reqs[i].tokenTimes[g - 1]);
        }
    }

    res.totalTokens = totalTokens;
    res.tp = (currentTime > 0) ? (static_cast<double>(totalTokens) / currentTime) : 0.0;

    double sumTdr = std::accumulate(tdrVec.begin(), tdrVec.end(), 0.0);
    res.tdr = tdrVec.empty() ? 0.0 : (sumTdr / tdrVec.size());

    double sumTpot = std::accumulate(tpotGaps.begin(), tpotGaps.end(), 0.0);
    res.tpot = tpotGaps.empty() ? 0.0 : (sumTpot / tpotGaps.size());

    double dist_tdr = std::max(0.0, res.tdr - sc.sc.SLO1);
    double dist_tpot = std::max(0.0, res.tpot - sc.sc.SLO2);
    res.dist = std::sqrt(dist_tdr * dist_tdr + dist_tpot * dist_tpot);

    res.tpScore = clampVal(res.tp, sc.sc.tp_base, sc.sc.tp_UB);
    if (sc.sc.dist_base > 0.0) {
        res.waitingScore = std::max(0.0, 1.0 - res.dist / sc.sc.dist_base);
    } else {
        res.waitingScore = (res.dist == 0.0) ? 1.0 : 0.0;
    }

    res.finalScore = 1000.0 * (sc.sc.w_tp * res.tpScore + sc.sc.w_c * res.waitingScore);
    return res;
}

struct SetStats {
    double mean = 0.0;
    double median = 0.0;
    double min_val = 0.0;
    double p5 = 0.0;
    double p10 = 0.0;
    double p25 = 0.0;
    double p75 = 0.0;
    double p95 = 0.0;
    double mean_tp = 0.0;
    double mean_waiting = 0.0;
    double mean_tdr = 0.0;
    double mean_tpot = 0.0;
    long total_violations = 0;

    int improved = 0;
    int unchanged = 0;
    int regressed = 0;

    int reg_gt_50 = 0;
    int reg_gt_100 = 0;
    int reg_gt_250 = 0;
    int reg_gt_500 = 0;
    int reg_gt_750 = 0;
};

SetStats computeStats(const std::vector<RunResult>& cand, const std::vector<RunResult>& v2) {
    SetStats st;
    size_t n = cand.size();
    if (n == 0) return st;

    std::vector<double> scores(n);
    double sum_score = 0.0, sum_tp = 0.0, sum_w = 0.0, sum_tdr = 0.0, sum_tpot = 0.0;

    for (size_t i = 0; i < n; ++i) {
        scores[i] = cand[i].finalScore;
        sum_score += cand[i].finalScore;
        sum_tp += cand[i].tpScore;
        sum_w += cand[i].waitingScore;
        sum_tdr += cand[i].tdr;
        sum_tpot += cand[i].tpot;
        st.total_violations += cand[i].violations;

        if (!v2.empty()) {
            double diff = cand[i].finalScore - v2[i].finalScore;
            if (diff > 0.01) st.improved++;
            else if (diff < -0.01) {
                st.regressed++;
                double reg = v2[i].finalScore - cand[i].finalScore;
                if (reg > 50) st.reg_gt_50++;
                if (reg > 100) st.reg_gt_100++;
                if (reg > 250) st.reg_gt_250++;
                if (reg > 500) st.reg_gt_500++;
                if (reg > 750) st.reg_gt_750++;
            } else st.unchanged++;
        }
    }

    std::sort(scores.begin(), scores.end());
    st.mean = sum_score / n;
    st.median = scores[n / 2];
    st.min_val = scores[0];
    st.p5 = scores[static_cast<size_t>(n * 0.05)];
    st.p10 = scores[static_cast<size_t>(n * 0.10)];
    st.p25 = scores[static_cast<size_t>(n * 0.25)];
    st.p75 = scores[static_cast<size_t>(n * 0.75)];
    st.p95 = scores[static_cast<size_t>(n * 0.95)];

    st.mean_tp = sum_tp / n;
    st.mean_waiting = sum_w / n;
    st.mean_tdr = sum_tdr / n;
    st.mean_tpot = sum_tpot / n;

    return st;
}

void printStatsHeader(const std::string& set_name) {
    std::cout << "\n=========================================================================================================\n";
    std::cout << "     STATISTICAL EVALUATION REPORT: " << set_name << "\n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left << std::setw(18) << "Strategy"
              << std::setw(10) << "Mean"
              << std::setw(10) << "Median"
              << std::setw(10) << "P5"
              << std::setw(10) << "P25"
              << std::setw(10) << "P75"
              << std::setw(10) << "P95"
              << std::setw(12) << "Mean TDR"
              << std::setw(12) << "Mean TPOT"
              << std::setw(10) << "Violations\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";
}

void printStrategyStatsRow(const std::string& name, const SetStats& st) {
    std::cout << std::left << std::setw(18) << name
              << std::setw(10) << std::fixed << std::setprecision(1) << st.mean
              << std::setw(10) << st.median
              << std::setw(10) << st.p5
              << std::setw(10) << st.p25
              << std::setw(10) << st.p75
              << std::setw(10) << st.p95
              << std::setw(12) << st.mean_tdr
              << std::setw(12) << st.mean_tpot
              << std::setw(10) << st.total_violations << "\n";
}

void printRegressionSummary(const std::string& name, const SetStats& st) {
    std::cout << "\n  [REGRESSION SUMMARY FOR " << name << " vs V2 GREEDY]\n";
    std::cout << "    Improved: " << st.improved << " | Unchanged: " << st.unchanged << " | Regressed: " << st.regressed << "\n";
    std::cout << "    Catastrophic Regressions:\n";
    std::cout << "      >50 pts: " << st.reg_gt_50 << " | >100 pts: " << st.reg_gt_100
              << " | >250 pts: " << st.reg_gt_250 << " | >500 pts: " << st.reg_gt_500
              << " | >750 pts: " << st.reg_gt_750 << "\n";
}

int main(int argc, char** argv) {
    int numScenarios = 1000;
    if (argc > 1) numScenarios = std::atoi(argv[1]);

    std::cout << "Generating " << numScenarios << " legal offline scenarios following problem.md spec...\n";

    std::vector<Scenario> scenarios(numScenarios);
    for (int i = 0; i < numScenarios; ++i) {
        scenarios[i] = ScenarioGenerator::generateScenario(100000 + i);
    }

    size_t trainCount = static_cast<size_t>(numScenarios * 0.60);
    size_t valCount = static_cast<size_t>(numScenarios * 0.20);
    size_t holdoutCount = numScenarios - trainCount - valCount;

    std::cout << "Split: " << trainCount << " Training | " << valCount << " Validation | " << holdoutCount << " Holdout\n";

    std::vector<RunResult> train_v1(trainCount), train_v2(trainCount), train_v3(trainCount);
    std::vector<RunResult> val_v1(valCount), val_v2(valCount), val_v3(valCount);
    std::vector<RunResult> hold_v1(holdoutCount), hold_v2(holdoutCount), hold_v3(holdoutCount);

    // Evaluate Training
    for (size_t i = 0; i < trainCount; ++i) {
        ReferenceStrategy stratV1;
        GreedyBatchStrategy stratV2(scenarios[i].table);
        AdaptiveStrategy stratV3(scenarios[i].table);

        train_v1[i] = simulateScenario(scenarios[i], &stratV1);
        train_v2[i] = simulateScenario(scenarios[i], &stratV2);
        train_v3[i] = simulateScenario(scenarios[i], &stratV3);
    }

    // Evaluate Validation
    for (size_t i = 0; i < valCount; ++i) {
        size_t idx = trainCount + i;
        ReferenceStrategy stratV1;
        GreedyBatchStrategy stratV2(scenarios[idx].table);
        AdaptiveStrategy stratV3(scenarios[idx].table);

        val_v1[i] = simulateScenario(scenarios[idx], &stratV1);
        val_v2[i] = simulateScenario(scenarios[idx], &stratV2);
        val_v3[i] = simulateScenario(scenarios[idx], &stratV3);
    }

    // Evaluate Holdout
    for (size_t i = 0; i < holdoutCount; ++i) {
        size_t idx = trainCount + valCount + i;
        ReferenceStrategy stratV1;
        GreedyBatchStrategy stratV2(scenarios[idx].table);
        AdaptiveStrategy stratV3(scenarios[idx].table);

        hold_v1[i] = simulateScenario(scenarios[idx], &stratV1);
        hold_v2[i] = simulateScenario(scenarios[idx], &stratV2);
        hold_v3[i] = simulateScenario(scenarios[idx], &stratV3);
    }

    // Print Training Stats
    SetStats st_tr_v1 = computeStats(train_v1, train_v2);
    SetStats st_tr_v2 = computeStats(train_v2, train_v2);
    SetStats st_tr_v3 = computeStats(train_v3, train_v2);
    printStatsHeader("TRAINING SPLIT (60%)");
    printStrategyStatsRow("V1 Reference", st_tr_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_tr_v2);
    printStrategyStatsRow("V3 Candidate", st_tr_v3);
    printRegressionSummary("V3 Candidate", st_tr_v3);

    // Print Validation Stats
    SetStats st_val_v1 = computeStats(val_v1, val_v2);
    SetStats st_val_v2 = computeStats(val_v2, val_v2);
    SetStats st_val_v3 = computeStats(val_v3, val_v2);
    printStatsHeader("UNTOUCHED VALIDATION SPLIT (20%)");
    printStrategyStatsRow("V1 Reference", st_val_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_val_v2);
    printStrategyStatsRow("V3 Candidate", st_val_v3);
    printRegressionSummary("V3 Candidate", st_val_v3);

    // Print Holdout Stats
    SetStats st_h_v1 = computeStats(hold_v1, hold_v2);
    SetStats st_h_v2 = computeStats(hold_v2, hold_v2);
    SetStats st_h_v3 = computeStats(hold_v3, hold_v2);
    printStatsHeader("UNTOUCHED HOLDOUT SPLIT (20%)");
    printStrategyStatsRow("V1 Reference", st_h_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_h_v2);
    printStrategyStatsRow("V3 Candidate", st_h_v3);
    printRegressionSummary("V3 Candidate", st_h_v3);

    std::cout << "\n=========================================================================================================\n\n";

    return 0;
}
