#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <iomanip>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>

struct AnalysisScenario {
    int id;
    std::string name;
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<std::pair<double, int>> arrivals; // (time, Lin)
    std::vector<int> Lout; // per request Lout
};

struct DetailedScoreResult {
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
    double meanDpreBatch = 0.0;
    int dprocGroups = 0;
    double meanDprocBatch = 0.0;
    int dpostGroups = 0;
    double meanDpostBatch = 0.0;
    
    bool success = false;
};

static double clampVal(double x, double baseVal, double targetVal) {
    if (targetVal == baseVal) return 0.0;
    double val = (x - baseVal) / (targetVal - baseVal);
    return std::max(0.0, std::min(1.0, val));
}

DetailedScoreResult runScenarioSimulation(const AnalysisScenario& scenario, const std::string& strategyFlag) {
    DetailedScoreResult res;

    int sim_to_solver[2];
    int solver_to_sim[2];

    if (pipe(sim_to_solver) < 0 || pipe(solver_to_sim) < 0) return res;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(sim_to_solver[0], STDIN_FILENO);
        dup2(solver_to_sim[1], STDOUT_FILENO);

        close(sim_to_solver[0]); close(sim_to_solver[1]);
        close(solver_to_sim[0]); close(solver_to_sim[1]);

        if (strategyFlag == "--ref") {
            execl("./solver", "./solver", "--ref", nullptr);
        } else if (strategyFlag == "--adaptive") {
            execl("./solver", "./solver", "--adaptive", nullptr);
        } else {
            execl("./solver", "./solver", nullptr);
        }
        exit(1);
    }

    close(sim_to_solver[0]);
    close(solver_to_sim[1]);

    FILE* out_to_solver = fdopen(sim_to_solver[1], "w");
    FILE* in_from_solver = fdopen(solver_to_sim[0], "r");

    fprintf(out_to_solver, "%d %.9f %.9f %.9f %lld %d\n", 
            scenario.sys.K, scenario.sys.S, scenario.sys.latency_in_ms, 
            scenario.sys.bandwidth_gbps, scenario.sys.bytes_per_token, scenario.sys.num_layers);

    fprintf(out_to_solver, "%.9f %.9f %.9f %.9f %.9f %.9f %.9f\n",
            scenario.sc.SLO1, scenario.sc.SLO2, scenario.sc.tp_UB,
            scenario.sc.tp_base, scenario.sc.dist_base, scenario.sc.w_tp, scenario.sc.w_c);

    fprintf(out_to_solver, "%d\n", scenario.table.N);
    for (const auto& row : scenario.table.raw_rows) {
        fprintf(out_to_solver, "%d %.9f %.9f %.9f %.9f %.9f %.9f\n",
                row.batch_size, row.prefill_pre, row.prefill_proc, row.prefill_post,
                row.decode_pre, row.decode_proc, row.decode_post);
    }
    fflush(out_to_solver);

    double t = 0.0;
    int nextArr = 0;
    int finishedRequests = 0;
    int totalRequests = scenario.arrivals.size();

    struct ReqTracker {
        int rid;
        double arrTime;
        int Lin;
        int Lout;
        int tokensDone = 0;
        int assignedRemote = -1;
        double ppostTime = 0.0;
        std::vector<double> tokenTimes;
    };
    std::map<int, ReqTracker> reqs;

    struct PendingEv {
        double time;
        std::string type;
        std::string payload;
        int rid;
        int count;
    };
    std::vector<PendingEv> pending;

    int totalDpreBatch = 0;
    int totalDprocBatch = 0;
    int totalDpostBatch = 0;

    int stepCount = 0;

    while (finishedRequests < totalRequests && stepCount < 200000) {
        stepCount++;
        std::vector<std::string> frameLines;

        while (nextArr < totalRequests && scenario.arrivals[nextArr].first == t) {
            reqs[nextArr] = {nextArr, t, scenario.arrivals[nextArr].second, scenario.Lout[nextArr], 0, -1, 0.0, {}};
            frameLines.push_back("ARR " + std::to_string(nextArr) + " " + std::to_string(scenario.arrivals[nextArr].second));
            nextArr++;
        }

        for (auto it = pending.begin(); it != pending.end(); ) {
            if (it->time == t) {
                frameLines.push_back(it->payload);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }

        fprintf(out_to_solver, "%.9f\n%zu\n", t, frameLines.size());
        for (const auto& line : frameLines) {
            fprintf(out_to_solver, "%s\n", line.c_str());
        }
        fflush(out_to_solver);

        int n = 0;
        if (fscanf(in_from_solver, "%d", &n) != 1) break;

        for (int i = 0; i < n; ++i) {
            char serverbuf[32], stepbuf[32], subbuf[32];
            fscanf(in_from_solver, "%s %s %s", serverbuf, stepbuf, subbuf);

            std::string server_str = serverbuf;
            std::string step = stepbuf;
            std::string sub = subbuf;

            if (step == "P" && sub == "PRE") {
                int remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d", &remote, &rid);
                reqs[rid].assignedRemote = remote;

                double dur = scenario.table.getDuration(TaskStep::PREFILL_PRE, reqs[rid].Lin);
                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " P PRE " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid, 1});

                long long bytes = reqs[rid].Lin * scenario.sys.bytes_per_token;
                double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                pending.push_back({t + scenario.sys.S + dur + trDur, "XDN_UP", "XDN UP " + std::to_string(remote) + " " + std::to_string(bytes) + " PRE 1 " + std::to_string(rid), rid, 1});

            } else if (step == "P" && sub == "PROC") {
                int ls = 0, le = 0, remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d %d %d", &ls, &le, &remote, &rid);
                double baseDur = scenario.table.getDuration(TaskStep::PREFILL_PROC, reqs[rid].Lin);
                double dur = (static_cast<double>(le - ls) / scenario.sys.num_layers) * baseDur;
                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " P PROC " + std::to_string(ls) + " " + std::to_string(le) + " " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid, 1});

                if (le == scenario.sys.num_layers) {
                    long long bytes = reqs[rid].Lin * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    pending.push_back({t + scenario.sys.S + dur + trDur, "XDN_DOWN", "XDN DOWN " + std::to_string(remote) + " " + std::to_string(bytes) + " PRE 1 " + std::to_string(rid), rid, 1});
                }

            } else if (step == "P" && sub == "POST") {
                int remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d", &remote, &rid);
                double dur = scenario.table.getDuration(TaskStep::PREFILL_POST, reqs[rid].Lin);
                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " P POST " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid, 1});
                reqs[rid].ppostTime = t + scenario.sys.S + dur;

            } else if (step == "D" && sub == "PRE") {
                int dummy = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &dummy, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                double dur = scenario.table.getDuration(TaskStep::DECODE_PRE, m);
                std::string ridsStr;
                for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " D PRE -1 " + std::to_string(m) + ridsStr + " " + std::to_string(dur), rids[0], m});

                res.dpreGroups++;
                totalDpreBatch += m;

                std::map<int, std::vector<int>> perRemote;
                for (int rid : rids) {
                    perRemote[reqs[rid].assignedRemote].push_back(rid);
                }
                for (const auto& kv : perRemote) {
                    int remote = kv.first;
                    int sub_m = kv.second.size();
                    long long bytes = sub_m * scenario.sys.bytes_per_token;
                    double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                    std::string subRidsStr;
                    for (int r : kv.second) subRidsStr += " " + std::to_string(r);
                    pending.push_back({t + scenario.sys.S + dur + trDur, "XDN_UP", "XDN UP " + std::to_string(remote) + " " + std::to_string(bytes) + " DEC " + std::to_string(sub_m) + subRidsStr, kv.second[0], sub_m});
                }

            } else if (step == "D" && sub == "PROC") {
                int remote = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &remote, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                double dur = scenario.table.getDuration(TaskStep::DECODE_PROC, m);
                std::string ridsStrProc;
                for (int r : rids) ridsStrProc += " " + std::to_string(r);
                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " D PROC " + std::to_string(remote) + " " + std::to_string(m) + ridsStrProc + " " + std::to_string(dur), rids[0], m});

                res.dprocGroups++;
                totalDprocBatch += m;

                long long bytes = m * scenario.sys.bytes_per_token;
                double trDur = scenario.sys.latency_in_ms + 8.0 * bytes / (scenario.sys.bandwidth_gbps * 1e6);
                pending.push_back({t + scenario.sys.S + dur + trDur, "XDN_DOWN", "XDN DOWN " + std::to_string(remote) + " " + std::to_string(bytes) + " DEC " + std::to_string(m) + ridsStrProc, rids[0], m});

            } else if (step == "D" && sub == "POST") {
                int dummy = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &dummy, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                double dur = scenario.table.getDuration(TaskStep::DECODE_POST, m);
                std::string ridsStrPost;
                for (int r : rids) ridsStrPost += " " + std::to_string(r);

                pending.push_back({t + scenario.sys.S + dur, "TDN", "TDN " + server_str + " D POST -1 " + std::to_string(m) + ridsStrPost + " " + std::to_string(dur), rids[0], m});

                res.dpostGroups++;
                totalDpostBatch += m;

                for (int rid : rids) {
                    reqs[rid].tokensDone++;
                    reqs[rid].tokenTimes.push_back(t + scenario.sys.S + dur);
                    if (reqs[rid].tokensDone == reqs[rid].Lout) {
                        finishedRequests++;
                        pending.push_back({t + scenario.sys.S + dur, "FIN", "FIN " + std::to_string(rid), rid, 1});
                    }
                }
            }
        }

        double nextT = 1e18;
        for (const auto& ev : pending) nextT = std::min(nextT, ev.time);
        if (nextArr < totalRequests) nextT = std::min(nextT, scenario.arrivals[nextArr].first);

        if (nextT >= 1e17) break;
        t = nextT;
    }

    fprintf(out_to_solver, "END\n");
    fflush(out_to_solver);

    fclose(out_to_solver);
    fclose(in_from_solver);

    int status = 0;
    waitpid(pid, &status, 0);

    if (finishedRequests == totalRequests) {
        res.success = true;
        
        double minArr = reqs[0].arrTime;
        double maxTokenTime = 0.0;
        long long totalTokens = 0;

        double sumTdr = 0.0;
        double sumTpotGaps = 0.0;
        long long tpotGapCount = 0;

        for (const auto& kv : reqs) {
            const auto& r = kv.second;
            totalTokens += r.Lout;
            minArr = std::min(minArr, r.arrTime);
            if (!r.tokenTimes.empty()) {
                maxTokenTime = std::max(maxTokenTime, r.tokenTimes.back());
            }
            sumTdr += (r.ppostTime - r.arrTime);

            for (size_t i = 0; i + 1 < r.tokenTimes.size(); ++i) {
                sumTpotGaps += (r.tokenTimes[i+1] - r.tokenTimes[i]);
                tpotGapCount++;
            }
        }

        res.totalTime = maxTokenTime - minArr;
        res.totalTokens = totalTokens;
        res.tp = (res.totalTime > 0) ? (totalTokens / res.totalTime) : 0.0;
        res.tdr = sumTdr / totalRequests;
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

        if (res.dpreGroups > 0) res.meanDpreBatch = static_cast<double>(totalDpreBatch) / res.dpreGroups;
        if (res.dprocGroups > 0) res.meanDprocBatch = static_cast<double>(totalDprocBatch) / res.dprocGroups;
        if (res.dpostGroups > 0) res.meanDpostBatch = static_cast<double>(totalDpostBatch) / res.dpostGroups;
    }

    return res;
}

// Scenario Generator for Full 10-Dimension Matrix Exploration
std::vector<AnalysisScenario> generateComprehensiveScenarioMatrix() {
    std::vector<AnalysisScenario> scenarios;
    int idCounter = 1;

    std::vector<int> K_list = {1, 2, 4, 8};
    std::vector<int> R_list = {2, 5, 10, 25, 50, 100};
    
    // Arrival patterns
    enum class ArrPattern { SIMULTANEOUS, EVEN, BURST, SPARSE, MIXED };
    std::vector<ArrPattern> patterns = { ArrPattern::SIMULTANEOUS, ArrPattern::EVEN, ArrPattern::BURST, ArrPattern::SPARSE, ArrPattern::MIXED };

    // Scoring profiles (w_tp, w_c)
    struct Profile { double w_tp; double w_c; std::string label; };
    std::vector<Profile> profiles = {
        {1.0, 0.0, "ProfA_TP1.0_WC0.0"},
        {0.9, 0.1, "ProfB_TP0.9_WC0.1"},
        {0.5, 0.5, "ProfC_TP0.5_WC0.5"},
        {0.1, 0.9, "ProfD_TP0.1_WC0.9"},
        {0.0, 1.0, "ProfE_TP0.0_WC1.0"}
    };

    // Dist base values
    std::vector<double> dist_bases = {2.0, 0.5, 0.0};

    // Generate representative set across the 10 dimensions
    for (int K : K_list) {
        for (int R : R_list) {
            for (auto pat : patterns) {
                for (const auto& prof : profiles) {
                    for (double db : dist_bases) {
                        AnalysisScenario sc;
                        sc.id = idCounter++;
                        std::string patName = (pat == ArrPattern::SIMULTANEOUS ? "Simult" : (pat == ArrPattern::EVEN ? "Even" : (pat == ArrPattern::BURST ? "Burst" : (pat == ArrPattern::SPARSE ? "Sparse" : "Mixed"))));
                        
                        sc.name = "Scen#" + std::to_string(sc.id) + " K=" + std::to_string(K) + " R=" + std::to_string(R) + " " + patName + " " + prof.label + " dist_base=" + (db == 0.0 ? "0.0" : (db == 0.5 ? "0.5" : "2.0"));

                        sc.sys.K = K;
                        sc.sys.S = 1.0;
                        sc.sys.num_layers = 4;
                        sc.sys.latency_in_ms = 1.0;
                        sc.sys.bandwidth_gbps = 2.0;
                        sc.sys.bytes_per_token = 125000;

                        sc.sc.w_tp = prof.w_tp;
                        sc.sc.w_c = prof.w_c;
                        sc.sc.dist_base = db;
                        sc.sc.tp_base = 0.01;
                        sc.sc.tp_UB = 0.50;

                        // Set SLOs (tight for small R, moderate for large R)
                        if (R <= 10) {
                            sc.sc.SLO1 = 40.0;
                            sc.sc.SLO2 = 12.0;
                        } else {
                            sc.sc.SLO1 = 150.0;
                            sc.sc.SLO2 = 25.0;
                        }

                        // Task Table (strongly favors batching vs linear)
                        sc.table.N = 4;
                        sc.table.raw_rows = {
                            {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
                            {4, 3.0, 10.0, 2.0, 1.5, 6.0, 1.5},
                            {16, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0},
                            {100, 3.0, 10.0, 2.0, 4.0, 16.0, 4.0}
                        };

                        // Generate arrivals
                        sc.arrivals.clear();
                        sc.Lout.clear();
                        for (int i = 0; i < R; ++i) {
                            double t_arr = 0.0;
                            if (pat == ArrPattern::SIMULTANEOUS) {
                                t_arr = 0.0;
                            } else if (pat == ArrPattern::EVEN) {
                                t_arr = i * 2.0;
                            } else if (pat == ArrPattern::BURST) {
                                t_arr = (i / 5) * 50.0;
                            } else if (pat == ArrPattern::SPARSE) {
                                t_arr = i * 40.0;
                            } else if (pat == ArrPattern::MIXED) {
                                t_arr = (i < R / 2) ? (i * 1.0) : (100.0 + (i - R / 2) * 30.0);
                            }
                            int Lin = (i % 2 == 0) ? 128 : 512;
                            int lout = (i % 3 == 0) ? 8 : ((i % 3 == 1) ? 32 : 64);
                            
                            sc.arrivals.push_back({t_arr, Lin});
                            sc.Lout.push_back(lout);
                        }

                        scenarios.push_back(sc);
                    }
                }
            }
        }
    }

    return scenarios;
}

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     EXHAUSTIVE OFFLINE SCORE ANALYSIS BENCHMARK (V1 vs V2 vs V3 Matrix)                  \n";
    std::cout << "=========================================================================================\n";

    auto scenarios = generateComprehensiveScenarioMatrix();
    std::cout << "Generated Matrix: " << scenarios.size() << " test scenarios across 10 dimensions.\n\n";

    struct Comparison {
        AnalysisScenario sc;
        DetailedScoreResult v1;
        DetailedScoreResult v2;
        DetailedScoreResult v3;
        double waitingLossV2; // max(0, v1.waitingScore - v2.waitingScore)
        double totalLossV2;   // max(0, v1.finalScore - v2.finalScore)
        double scoreDeltaV3V2; // v3 - v2
        bool distZeroDiscontinuity;
    };

    std::vector<Comparison> comparisons;

    int step = scenarios.size() / 120;
    if (step < 1) step = 1;

    int runCount = 0;
    for (size_t i = 0; i < scenarios.size(); i += step) {
        const auto& sc = scenarios[i];
        
        DetailedScoreResult v1 = runScenarioSimulation(sc, "--ref");
        DetailedScoreResult v2 = runScenarioSimulation(sc, "--greedy");
        DetailedScoreResult v3 = runScenarioSimulation(sc, "--adaptive");

        if (v1.success && v2.success && v3.success) {
            Comparison comp;
            comp.sc = sc;
            comp.v1 = v1;
            comp.v2 = v2;
            comp.v3 = v3;
            comp.waitingLossV2 = v1.waitingScore - v2.waitingScore;
            comp.totalLossV2 = v1.finalScore - v2.finalScore;
            comp.scoreDeltaV3V2 = v3.finalScore - v2.finalScore;
            comp.distZeroDiscontinuity = (sc.sc.dist_base == 0.0 && v2.dist > 0.0 && v2.waitingScore == 0.0 && v1.waitingScore > 0.0);

            comparisons.push_back(comp);
            runCount++;
        }
    }

    std::cout << "Successfully analyzed " << runCount << " scenarios.\n\n";

    // 1. Top Scenarios where V2 loses waiting score
    std::sort(comparisons.begin(), comparisons.end(), [](const Comparison& a, const Comparison& b) {
        return a.waitingLossV2 > b.waitingLossV2;
    });

    std::cout << "=========================================================================================\n";
    std::cout << "TOP SCENARIOS WHERE V2 GREEDY LOSES WAITING SCORE (VS V1 REFERENCE)\n";
    std::cout << "=========================================================================================\n";
    std::cout << std::left << std::setw(8) << "ScenID" << std::setw(40) << "Scenario Name" << std::setw(12) << "V1 WaitSc" << std::setw(12) << "V2 WaitSc" << std::setw(12) << "Wait Loss" << std::setw(12) << "V2 TPOT" << "SLO2\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";
    for (size_t i = 0; i < std::min<size_t>(10, comparisons.size()); ++i) {
        const auto& c = comparisons[i];
        std::cout << std::left << std::setw(8) << c.sc.id << std::setw(40) << c.sc.name.substr(0, 38) << std::setw(12) << c.v1.waitingScore << std::setw(12) << c.v2.waitingScore << std::setw(12) << c.waitingLossV2 << std::setw(12) << c.v2.tpot << c.sc.sc.SLO2 << "\n";
    }

    // 2. Top Scenarios where V2 loses total score
    std::sort(comparisons.begin(), comparisons.end(), [](const Comparison& a, const Comparison& b) {
        return a.totalLossV2 > b.totalLossV2;
    });

    std::cout << "\n=========================================================================================\n";
    std::cout << "TOP SCENARIOS WHERE V2 GREEDY LOSES TOTAL SCORE (VS V1 REFERENCE)\n";
    std::cout << "=========================================================================================\n";
    std::cout << std::left << std::setw(8) << "ScenID" << std::setw(40) << "Scenario Name" << std::setw(12) << "V1 Score" << std::setw(12) << "V2 Score" << std::setw(12) << "Score Loss" << std::setw(12) << "V2 TDR" << "SLO1\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";
    for (size_t i = 0; i < std::min<size_t>(10, comparisons.size()); ++i) {
        const auto& c = comparisons[i];
        std::cout << std::left << std::setw(8) << c.sc.id << std::setw(40) << c.sc.name.substr(0, 38) << std::setw(12) << c.v1.finalScore << std::setw(12) << c.v2.finalScore << std::setw(12) << c.totalLossV2 << std::setw(12) << c.v2.tdr << c.sc.sc.SLO1 << "\n";
    }

    // 3. V3 vs V2 Decision & Score Difference Analysis
    int v3v2DiffCount = 0;
    for (const auto& c : comparisons) {
        if (std::abs(c.scoreDeltaV3V2) > 1e-4) v3v2DiffCount++;
    }

    std::cout << "\n=========================================================================================\n";
    std::cout << "V3 ADAPTIVE VS V2 GREEDY DIFFERENCE SUMMARY\n";
    std::cout << "=========================================================================================\n";
    std::cout << "Total Scenarios Tested: " << comparisons.size() << "\n";
    std::cout << "Scenarios where V3 Score != V2 Score: " << v3v2DiffCount << " (" << (100.0 * v3v2DiffCount / comparisons.size()) << "%)\n";

    return 0;
}
