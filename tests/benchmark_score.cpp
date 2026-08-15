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

struct BenchmarkScenario {
    std::string name;
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<std::pair<double, int>> arrivals;
    int Lout;
};

struct ScenarioScoreResult {
    double totalTime = 0.0;
    double tp = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
    double tpScore = 0.0;
    double waitingScore = 0.0;
    double finalScore = 0.0;
    bool success = false;
};

double clampVal(double x, double baseVal, double targetVal) {
    if (targetVal == baseVal) return 0.0;
    double val = (x - baseVal) / (targetVal - baseVal);
    return std::max(0.0, std::min(1.0, val));
}

ScenarioScoreResult runStrategyBenchmark(const BenchmarkScenario& scenario, const std::string& strategyFlag) {
    ScenarioScoreResult res;

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

    int stepCount = 0;

    while (finishedRequests < totalRequests && stepCount < 200000) {
        stepCount++;

        std::vector<std::string> frameLines;

        while (nextArr < totalRequests && scenario.arrivals[nextArr].first == t) {
            reqs[nextArr] = {nextArr, t, scenario.arrivals[nextArr].second, scenario.Lout, 0, -1, 0.0, {}};
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
        res.tp = (res.totalTime > 0) ? (totalTokens / res.totalTime) : 0.0;
        res.tdr = sumTdr / totalRequests;
        res.tpot = (tpotGapCount > 0) ? (sumTpotGaps / tpotGapCount) : 0.0;

        res.tpScore = 1000.0 * scenario.sc.w_tp * clampVal(res.tp, scenario.sc.tp_base, scenario.sc.tp_UB);

        double excessTdr = std::max(0.0, (res.tdr - scenario.sc.SLO1) / scenario.sc.SLO1);
        double excessTpot = std::max(0.0, (res.tpot - scenario.sc.SLO2) / scenario.sc.SLO2);
        double dist = std::sqrt(excessTdr * excessTdr + excessTpot * excessTpot);
        
        double waitingComp = 0.0;
        if (scenario.sc.dist_base > 0) {
            waitingComp = clampVal(dist, scenario.sc.dist_base, 0.0);
        } else {
            waitingComp = (dist == 0.0) ? 1.0 : 0.0;
        }
        res.waitingScore = 1000.0 * scenario.sc.w_c * waitingComp;

        res.finalScore = res.tpScore + res.waitingScore;
    }

    return res;
}

void printBenchmarkResult(const BenchmarkScenario& sc) {
    auto v1 = runStrategyBenchmark(sc, "--ref");
    auto v2 = runStrategyBenchmark(sc, "--greedy");
    auto v3 = runStrategyBenchmark(sc, "--adaptive");

    std::cout << "\nScenario: " << sc.name << "\n";
    std::cout << std::left << std::setw(22) << "Metric" 
              << std::setw(18) << "V1 (Reference)" 
              << std::setw(18) << "V2 (Greedy Batch)" 
              << std::setw(18) << "V3 (Adaptive)"
              << "V3 Improvement\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";
    std::cout << std::setw(22) << "Total Time (ms)" << std::setw(18) << v1.totalTime << std::setw(18) << v2.totalTime << std::setw(18) << v3.totalTime << (v3.totalTime - v1.totalTime) << " ms\n";
    std::cout << std::setw(22) << "Tokens/ms" << std::setw(18) << v1.tp << std::setw(18) << v2.tp << std::setw(18) << v3.tp << "+" << (v3.tp - v1.tp) << "\n";
    std::cout << std::setw(22) << "Mean TDR (ms)" << std::setw(18) << v1.tdr << std::setw(18) << v2.tdr << std::setw(18) << v3.tdr << (v3.tdr - v1.tdr) << " ms\n";
    std::cout << std::setw(22) << "Mean TPOT (ms)" << std::setw(18) << v1.tpot << std::setw(18) << v2.tpot << std::setw(18) << v3.tpot << (v3.tpot - v1.tpot) << " ms\n";
    std::cout << std::setw(22) << "Throughput Score" << std::setw(18) << v1.tpScore << std::setw(18) << v2.tpScore << std::setw(18) << v3.tpScore << "+" << (v3.tpScore - v1.tpScore) << "\n";
    std::cout << std::setw(22) << "Waiting Score" << std::setw(18) << v1.waitingScore << std::setw(18) << v2.waitingScore << std::setw(18) << v3.waitingScore << "+" << (v3.waitingScore - v1.waitingScore) << "\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";
    std::cout << std::setw(22) << "FINAL SCORE (/1000)" << std::setw(18) << v1.finalScore << std::setw(18) << v2.finalScore << std::setw(18) << v3.finalScore << "\033[1;32m+" << (v3.finalScore - v1.finalScore) << "\033[0m\n";
}

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     STRATEGY ABLATION MATRIX: V1 Baseline vs V2 Greedy vs V3 Adaptive Strategy           \n";
    std::cout << "=========================================================================================\n";

    // Scenario 1: Balanced w_tp = 0.5, w_c = 0.5
    BenchmarkScenario sc1;
    sc1.name = "Scenario 1: K=4, R=20, num_layers=4 (Balanced w_tp=0.5, w_c=0.5)";
    sc1.sys.K = 4; sc1.sys.S = 1.0; sc1.sys.num_layers = 4;
    sc1.sys.latency_in_ms = 2.0; sc1.sys.bandwidth_gbps = 1.0; sc1.sys.bytes_per_token = 125000;
    sc1.sc.SLO1 = 30.0; sc1.sc.SLO2 = 15.0; sc1.sc.tp_UB = 0.20; sc1.sc.tp_base = 0.02; sc1.sc.dist_base = 2.0;
    sc1.sc.w_tp = 0.5; sc1.sc.w_c = 0.5;
    sc1.table.N = 4;
    sc1.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {4, 3.0, 10.0, 2.0, 1.5, 6.0, 1.5}, {8, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0}, {20, 3.0, 10.0, 2.0, 3.0, 12.0, 3.0}};
    for (int i = 0; i < 20; ++i) sc1.arrivals.push_back({i * 0.2, 4});
    sc1.Lout = 8;
    printBenchmarkResult(sc1);

    // Scenario 2: Latency Sensitive w_c = 0.9, w_tp = 0.1, tight SLO targets
    BenchmarkScenario sc2;
    sc2.name = "Scenario 2: Latency Sensitive (w_c=0.9, w_tp=0.1, tight SLO1=150, SLO2=30)";
    sc2.sys.K = 4; sc2.sys.S = 1.0; sc2.sys.num_layers = 4;
    sc2.sys.latency_in_ms = 1.0; sc2.sys.bandwidth_gbps = 2.0; sc2.sys.bytes_per_token = 125000;
    sc2.sc.SLO1 = 150.0; sc2.sc.SLO2 = 30.0; sc2.sc.tp_UB = 0.30; sc2.sc.tp_base = 0.02; sc2.sc.dist_base = 1.0;
    sc2.sc.w_tp = 0.1; sc2.sc.w_c = 0.9;
    sc2.table.N = 4;
    sc2.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {4, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0}, {8, 3.0, 10.0, 2.0, 4.0, 16.0, 4.0}, {20, 3.0, 10.0, 2.0, 8.0, 32.0, 8.0}};
    for (int i = 0; i < 20; ++i) sc2.arrivals.push_back({i * 0.1, 4});
    sc2.Lout = 10;
    printBenchmarkResult(sc2);

    // Scenario 3: High Throughput w_tp = 0.9, w_c = 0.1
    BenchmarkScenario sc3;
    sc3.name = "Scenario 3: High Throughput (w_tp=0.9, w_c=0.1, high bandwidth)";
    sc3.sys.K = 8; sc3.sys.S = 1.0; sc3.sys.num_layers = 4;
    sc3.sys.latency_in_ms = 0.5; sc3.sys.bandwidth_gbps = 5.0; sc3.sys.bytes_per_token = 125000;
    sc3.sc.SLO1 = 300.0; sc3.sc.SLO2 = 50.0; sc3.sc.tp_UB = 0.50; sc3.sc.tp_base = 0.05; sc3.sc.dist_base = 2.0;
    sc3.sc.w_tp = 0.9; sc3.sc.w_c = 0.1;
    sc3.table.N = 4;
    sc3.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {4, 3.0, 10.0, 2.0, 1.2, 4.8, 1.2}, {8, 3.0, 10.0, 2.0, 1.5, 6.0, 1.5}, {32, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0}};
    for (int i = 0; i < 40; ++i) sc3.arrivals.push_back({i * 0.05, 4});
    sc3.Lout = 15;
    printBenchmarkResult(sc3);

    std::cout << "=========================================================================================\n";
    return 0;
}
