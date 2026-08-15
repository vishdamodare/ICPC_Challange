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
#include <thread>
#include <future>

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
    int num_layers = sc.sys.num_layers;

    struct ReqState {
        int rid;
        double arrTime;
        int Lin;
        int Lout;
        int stage = 0;
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
        int type;
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
            evStruct.server[0] = 'E'; evStruct.server[1] = '\0';
            memcpy(evStruct.task_spec, ev.taskSpec.c_str(), std::min<size_t>(ev.taskSpec.length() + 1, sizeof(evStruct.task_spec)));
        } else if (ev.type == 2) {
            evStruct.type = EventType::TDN;
            std::string sName = "C" + std::to_string(ev.server);
            memcpy(evStruct.server, sName.c_str(), std::min<size_t>(sName.length() + 1, sizeof(evStruct.server)));
            memcpy(evStruct.task_spec, ev.taskSpec.c_str(), std::min<size_t>(ev.taskSpec.length() + 1, sizeof(evStruct.task_spec)));
        } else if (ev.type == 3) {
            evStruct.type = EventType::XDN;
            evStruct.direction[0] = 'U'; evStruct.direction[1] = 'P'; evStruct.direction[2] = '\0';
            std::string tag = (ev.m > 1) ? "DEC" : "PRE";
            memcpy(evStruct.stage_tag, tag.c_str(), std::min<size_t>(tag.length() + 1, sizeof(evStruct.stage_tag)));
            evStruct.m = ev.m;
            for (int r = 0; r < std::min<int>(ev.m, 64); ++r) evStruct.rids[r] = ev.rids[r];
        } else if (ev.type == 4) {
            evStruct.type = EventType::XDN;
            evStruct.direction[0] = 'D'; evStruct.direction[1] = 'O'; evStruct.direction[2] = 'W'; evStruct.direction[3] = 'N'; evStruct.direction[4] = '\0';
            std::string tag = (ev.m > 1) ? "DEC" : "PRE";
            memcpy(evStruct.stage_tag, tag.c_str(), std::min<size_t>(tag.length() + 1, sizeof(evStruct.stage_tag)));
            evStruct.m = ev.m;
            for (int r = 0; r < std::min<int>(ev.m, 64); ++r) evStruct.rids[r] = ev.rids[r];
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
    double p1 = 0.0;
    double p5 = 0.0;
    double p10 = 0.0;
    double p25 = 0.0;
    double p50 = 0.0;
    double p75 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double min_val = 0.0;
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
    st.min_val = scores[0];
    st.p1 = scores[static_cast<size_t>(n * 0.01)];
    st.p5 = scores[static_cast<size_t>(n * 0.05)];
    st.p10 = scores[static_cast<size_t>(n * 0.10)];
    st.p25 = scores[static_cast<size_t>(n * 0.25)];
    st.p50 = scores[n / 2];
    st.median = st.p50;
    st.p75 = scores[static_cast<size_t>(n * 0.75)];
    st.p90 = scores[static_cast<size_t>(n * 0.90)];
    st.p95 = scores[static_cast<size_t>(n * 0.95)];

    st.mean_tp = sum_tp / n;
    st.mean_waiting = sum_w / n;
    st.mean_tdr = sum_tdr / n;
    st.mean_tpot = sum_tpot / n;

    return st;
}

void printStatsHeader(const std::string& set_name) {
    std::cout << "\n=========================================================================================================================\n";
    std::cout << "     100,000 SCENARIO EVALUATION REPORT: " << set_name << "\n";
    std::cout << "=========================================================================================================================\n";
    std::cout << std::left << std::setw(18) << "Strategy"
              << std::setw(9) << "Mean"
              << std::setw(9) << "P1"
              << std::setw(9) << "P5"
              << std::setw(9) << "P10"
              << std::setw(9) << "P25"
              << std::setw(9) << "P50"
              << std::setw(9) << "P75"
              << std::setw(9) << "P90"
              << std::setw(9) << "P95"
              << std::setw(11) << "Mean TDR"
              << std::setw(11) << "Mean TPOT"
              << std::setw(9) << "Violations\n";
    std::cout << "-------------------------------------------------------------------------------------------------------------------------\n";
}

void printStrategyStatsRow(const std::string& name, const SetStats& st) {
    std::cout << std::left << std::setw(18) << name
              << std::setw(9) << std::fixed << std::setprecision(1) << st.mean
              << std::setw(9) << st.p1
              << std::setw(9) << st.p5
              << std::setw(9) << st.p10
              << std::setw(9) << st.p25
              << std::setw(9) << st.p50
              << std::setw(9) << st.p75
              << std::setw(9) << st.p90
              << std::setw(9) << st.p95
              << std::setw(11) << st.mean_tdr
              << std::setw(11) << st.mean_tpot
              << std::setw(9) << st.total_violations << "\n";
}

void printRegressionSummary(const std::string& name, const SetStats& st) {
    std::cout << "\n  [REGRESSION BREAKDOWN FOR " << name << " vs V2 GREEDY BASELINE]\n";
    std::cout << "    Improved: " << st.improved << " | Unchanged: " << st.unchanged << " | Regressed: " << st.regressed << "\n";
    std::cout << "    Catastrophic Regressions:\n";
    std::cout << "      >50 pts: " << st.reg_gt_50 << " | >100 pts: " << st.reg_gt_100
              << " | >250 pts: " << st.reg_gt_250 << " | >500 pts: " << st.reg_gt_500
              << " | >750 pts: " << st.reg_gt_750 << "\n";
}

int main(int argc, char** argv) {
    int totalScenarios = 100000;
    if (argc > 1) totalScenarios = std::atoi(argv[1]);

    std::vector<uint32_t> seeds = {42, 123, 2026, 8675309, 314159};
    size_t scenariosPerSeed = totalScenarios / seeds.size();

    std::cout << "Generating " << totalScenarios << " legal scenarios across " << seeds.size() << " independent seeds...\n";

    std::vector<Scenario> scenarios(totalScenarios);
    for (size_t s = 0; s < seeds.size(); ++s) {
        for (size_t i = 0; i < scenariosPerSeed; ++i) {
            size_t idx = s * scenariosPerSeed + i;
            scenarios[idx] = ScenarioGenerator::generateScenario(seeds[s] + i);
        }
    }

    size_t trainCount = static_cast<size_t>(totalScenarios * 0.60);
    size_t valCount = static_cast<size_t>(totalScenarios * 0.20);
    size_t holdoutCount = totalScenarios - trainCount - valCount;

    std::cout << "Split: " << trainCount << " Training (60%) | " << valCount << " Validation (20%) | " << holdoutCount << " Untouched Holdout (20%)\n";

    std::vector<RunResult> train_v1(trainCount), train_v2(trainCount), train_v3(trainCount);
    std::vector<RunResult> val_v1(valCount), val_v2(valCount), val_v3(valCount);
    std::vector<RunResult> hold_v1(holdoutCount), hold_v2(holdoutCount), hold_v3(holdoutCount);

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;
    std::cout << "Executing evaluation across " << numThreads << " worker threads...\n";

    // Evaluate Training Split in Parallel
    auto evalRange = [&](size_t startIdx, size_t count, std::vector<RunResult>& resV1, std::vector<RunResult>& resV2, std::vector<RunResult>& resV3) {
        for (size_t i = 0; i < count; ++i) {
            size_t scIdx = startIdx + i;
            ReferenceStrategy stratV1;
            GreedyBatchStrategy stratV2(scenarios[scIdx].table);
            AdaptiveStrategy stratV3(scenarios[scIdx].table);

            resV1[i] = simulateScenario(scenarios[scIdx], &stratV1);
            resV2[i] = simulateScenario(scenarios[scIdx], &stratV2);
            resV3[i] = simulateScenario(scenarios[scIdx], &stratV3);
        }
    };

    auto runParallelEval = [&](size_t startIdx, size_t count, std::vector<RunResult>& resV1, std::vector<RunResult>& resV2, std::vector<RunResult>& resV3) {
        std::vector<std::thread> workers;
        size_t chunkSize = count / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            size_t subStart = t * chunkSize;
            size_t subCount = (t == numThreads - 1) ? (count - subStart) : chunkSize;
            workers.emplace_back([&, subStart, subCount]() {
                for (size_t i = 0; i < subCount; ++i) {
                    size_t localIdx = subStart + i;
                    size_t scIdx = startIdx + localIdx;
                    ReferenceStrategy stratV1;
                    GreedyBatchStrategy stratV2(scenarios[scIdx].table);
                    AdaptiveStrategy stratV3(scenarios[scIdx].table);

                    resV1[localIdx] = simulateScenario(scenarios[scIdx], &stratV1);
                    resV2[localIdx] = simulateScenario(scenarios[scIdx], &stratV2);
                    resV3[localIdx] = simulateScenario(scenarios[scIdx], &stratV3);
                }
            });
        }
        for (auto& w : workers) w.join();
    };

    std::cout << "  Evaluating Training Split (60,000 scenarios)..." << std::flush;
    runParallelEval(0, trainCount, train_v1, train_v2, train_v3);
    std::cout << " DONE\n";

    std::cout << "  Evaluating Validation Split (20,000 scenarios)..." << std::flush;
    runParallelEval(trainCount, valCount, val_v1, val_v2, val_v3);
    std::cout << " DONE\n";

    std::cout << "  Evaluating Untouched Holdout Split (20,000 scenarios)..." << std::flush;
    runParallelEval(trainCount + valCount, holdoutCount, hold_v1, hold_v2, hold_v3);
    std::cout << " DONE\n";

    // Print Training Stats
    SetStats st_tr_v1 = computeStats(train_v1, train_v2);
    SetStats st_tr_v2 = computeStats(train_v2, train_v2);
    SetStats st_tr_v3 = computeStats(train_v3, train_v2);
    printStatsHeader("TRAINING SPLIT (60,000 SCENARIOS)");
    printStrategyStatsRow("V1 Reference", st_tr_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_tr_v2);
    printStrategyStatsRow("V3 Candidate", st_tr_v3);
    printRegressionSummary("V3 Candidate", st_tr_v3);

    // Print Validation Stats
    SetStats st_val_v1 = computeStats(val_v1, val_v2);
    SetStats st_val_v2 = computeStats(val_v2, val_v2);
    SetStats st_val_v3 = computeStats(val_v3, val_v2);
    printStatsHeader("UNTOUCHED VALIDATION SPLIT (20,000 SCENARIOS)");
    printStrategyStatsRow("V1 Reference", st_val_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_val_v2);
    printStrategyStatsRow("V3 Candidate", st_val_v3);
    printRegressionSummary("V3 Candidate", st_val_v3);

    // Print Holdout Stats
    SetStats st_h_v1 = computeStats(hold_v1, hold_v2);
    SetStats st_h_v2 = computeStats(hold_v2, hold_v2);
    SetStats st_h_v3 = computeStats(hold_v3, hold_v2);
    printStatsHeader("UNTOUCHED HOLDOUT SPLIT (20,000 SCENARIOS)");
    printStrategyStatsRow("V1 Reference", st_h_v1);
    printStrategyStatsRow("V2 Greedy (Safe)", st_h_v2);
    printStrategyStatsRow("V3 Candidate", st_h_v3);
    printRegressionSummary("V3 Candidate", st_h_v3);

    std::cout << "\n=========================================================================================================================\n\n";

    return 0;
}
