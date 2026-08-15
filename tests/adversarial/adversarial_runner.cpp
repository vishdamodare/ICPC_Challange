#include "../../src/protocol.hpp"
#include "../../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>

struct TestScenario {
    std::string name;
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<std::pair<double, int>> arrivals; // {time, Lin}
    int Lout;
};

bool runSingleAdversarialTest(const TestScenario& scenario, bool verbose = false) {
    int sim_to_solver[2];
    int solver_to_sim[2];

    if (pipe(sim_to_solver) < 0 || pipe(solver_to_sim) < 0) return false;

    pid_t pid = fork();
    if (pid == 0) {
        // Child: Solver process
        dup2(sim_to_solver[0], STDIN_FILENO);
        dup2(solver_to_sim[1], STDOUT_FILENO);

        close(sim_to_solver[0]); close(sim_to_solver[1]);
        close(solver_to_sim[0]); close(solver_to_sim[1]);

        execl("./solver", "./solver", nullptr);
        exit(1);
    }

    // Parent: Simulator
    close(sim_to_solver[0]);
    close(solver_to_sim[1]);

    FILE* out_to_solver = fdopen(sim_to_solver[1], "w");
    FILE* in_from_solver = fdopen(solver_to_sim[0], "r");

    // Output config
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

    struct LocalReq {
        int rid;
        double arrTime;
        int Lin;
        int Lout;
        int tokensDone = 0;
        int assignedRemote = -1;
    };
    std::map<int, LocalReq> reqs;

    struct PendingEv {
        double time;
        std::string type;
        std::string payload;
        int rid;
        int count;
    };
    std::vector<PendingEv> pending;

    bool stuck = false;
    int stepCount = 0;

    while (finishedRequests < totalRequests && stepCount < 200000) {
        stepCount++;

        std::vector<std::string> frameLines;

        // Arrivals
        while (nextArr < totalRequests && scenario.arrivals[nextArr].first == t) {
            reqs[nextArr] = {nextArr, t, scenario.arrivals[nextArr].second, scenario.Lout, 0, -1};
            frameLines.push_back("ARR " + std::to_string(nextArr) + " " + std::to_string(scenario.arrivals[nextArr].second));
            nextArr++;
        }

        // Pending events
        for (auto it = pending.begin(); it != pending.end(); ) {
            if (it->time == t) {
                frameLines.push_back(it->payload);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }

        // Print frame
        fprintf(out_to_solver, "%.9f\n%zu\n", t, frameLines.size());
        for (const auto& line : frameLines) {
            fprintf(out_to_solver, "%s\n", line.c_str());
        }
        fflush(out_to_solver);

        int n = 0;
        if (fscanf(in_from_solver, "%d", &n) != 1) {
            stuck = true;
            break;
        }

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
        if (n == 0 && pending.empty() && nextArr >= totalRequests) {
            break;
        }
    }

    fprintf(out_to_solver, "END\n");
    fflush(out_to_solver);

    fclose(out_to_solver);
    fclose(in_from_solver);

    int status = 0;
    waitpid(pid, &status, 0);

    if (finishedRequests < totalRequests) {
        std::cerr << " [DEBUG] " << scenario.name << ": Finished " << finishedRequests << "/" << totalRequests << " requests. StepCount=" << stepCount << ", Stuck=" << stuck << "\n";
    }

    return (finishedRequests == totalRequests && !stuck);
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "     COMPREHENSIVE ADVERSARIAL PROTOCOL TESTS        \n";
    std::cout << "=====================================================\n";

    // Test A: K=1 Single Cloud
    TestScenario testA;
    testA.name = "Test A: K=1 Single Cloud";
    testA.sys.K = 1; testA.sys.S = 1.0; testA.sys.num_layers = 4;
    testA.table.N = 1; testA.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}};
    testA.arrivals = {{0.0, 4}}; testA.Lout = 2;
    std::cout << "  [RUNNING] " << testA.name << " ... " << (runSingleAdversarialTest(testA) ? "[PASS]" : "[FAIL]") << "\n";

    // Test B: Single Layer num_layers=1
    TestScenario testB;
    testB.name = "Test B: num_layers=1 Degenerate Case";
    testB.sys.K = 2; testB.sys.S = 1.0; testB.sys.num_layers = 1;
    testB.table.N = 1; testB.table.raw_rows = {{1, 2.0, 5.0, 1.0, 1.0, 2.0, 1.0}};
    testB.arrivals = {{0.0, 2}, {1.0, 4}}; testB.Lout = 3;
    std::cout << "  [RUNNING] " << testB.name << " ... " << (runSingleAdversarialTest(testB) ? "[PASS]" : "[FAIL]") << "\n";

    // Test C: Multi-Cloud K=4
    TestScenario testC;
    testC.name = "Test C: Multi-Cloud K=4";
    testC.sys.K = 4; testC.sys.S = 1.0; testC.sys.num_layers = 8;
    testC.table.N = 1; testC.table.raw_rows = {{1, 3.0, 12.0, 2.0, 1.0, 5.0, 1.0}};
    testC.arrivals = {{0.0, 4}, {0.5, 4}, {1.0, 4}, {1.5, 4}}; testC.Lout = 4;
    std::cout << "  [RUNNING] " << testC.name << " ... " << (runSingleAdversarialTest(testC) ? "[PASS]" : "[FAIL]") << "\n";

    // Test D: 1-Token Requests
    TestScenario testD;
    testD.name = "Test D: 1-Token Requests (No TPOT gap)";
    testD.sys.K = 2; testD.sys.S = 1.0; testD.sys.num_layers = 4;
    testD.table.N = 1; testD.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}};
    testD.arrivals = {{0.0, 2}, {0.1, 2}}; testD.Lout = 1;
    std::cout << "  [RUNNING] " << testD.name << " ... " << (runSingleAdversarialTest(testD) ? "[PASS]" : "[FAIL]") << "\n";

    // Test E: Cross-Cloud D PRE Grouping
    TestScenario testE;
    testE.name = "Test E: Cross-Cloud D PRE Grouping";
    testE.sys.K = 3; testE.sys.S = 1.0; testE.sys.num_layers = 4;
    testE.table.N = 2; testE.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {3, 3.0, 10.0, 2.0, 2.0, 6.0, 2.0}};
    testE.arrivals = {{0.0, 4}, {0.0, 4}, {0.0, 4}}; testE.Lout = 2;
    std::cout << "  [RUNNING] " << testE.name << " ... " << (runSingleAdversarialTest(testE) ? "[PASS]" : "[FAIL]") << "\n";

    // Test F: D PROC Regrouping Across D PREs
    TestScenario testF;
    testF.name = "Test F: D PROC Regrouping Across D PRE Groups";
    testF.sys.K = 2; testF.sys.S = 1.0; testF.sys.num_layers = 4;
    testF.table.N = 2; testF.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {2, 3.0, 10.0, 2.0, 1.5, 5.0, 1.5}};
    testF.arrivals = {{0.0, 4}, {0.1, 4}, {0.2, 4}, {0.3, 4}}; testF.Lout = 3;
    std::cout << "  [RUNNING] " << testF.name << " ... " << (runSingleAdversarialTest(testF) ? "[PASS]" : "[FAIL]") << "\n";

    // Test G: D POST Regrouping Across D PROCs
    TestScenario testG;
    testG.name = "Test G: D POST Regrouping Across D PROC Groups";
    testG.sys.K = 4; testG.sys.S = 1.0; testG.sys.num_layers = 4;
    testG.table.N = 2; testG.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {4, 3.0, 10.0, 2.0, 2.0, 7.0, 2.0}};
    testG.arrivals = {{0.0, 4}, {0.0, 4}, {0.0, 4}, {0.0, 4}}; testG.Lout = 2;
    std::cout << "  [RUNNING] " << testG.name << " ... " << (runSingleAdversarialTest(testG) ? "[PASS]" : "[FAIL]") << "\n";

    // Test H: High Volume Stress (R=500, K=8)
    TestScenario testH;
    testH.name = "Test H: High Volume Stress (R=500, K=8)";
    testH.sys.K = 8; testH.sys.S = 1.0; testH.sys.num_layers = 4;
    testH.table.N = 2; testH.table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}, {16, 3.0, 10.0, 2.0, 4.0, 12.0, 4.0}};
    for (int i = 0; i < 500; ++i) {
        testH.arrivals.push_back({i * 0.1, 4});
    }
    testH.Lout = 5;
    std::cout << "  [RUNNING] " << testH.name << " ... " << (runSingleAdversarialTest(testH) ? "[PASS]" : "[FAIL]") << "\n";

    std::cout << "=====================================================\n";
    std::cout << "          ALL ADVERSARIAL TESTS COMPLETED            \n";
    std::cout << "=====================================================\n";

    return 0;
}
