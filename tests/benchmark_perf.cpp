#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <sys/resource.h>
#include <iomanip>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <algorithm>

struct PerfPendingEv {
    double time;
    std::string type;
    std::string payload;
    int rid;
};

void runPerformanceBenchmark(int numRequests, int numTokens) {
    SystemConfig sys;
    sys.K = 8; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    ScoringConfig sc;
    sc.SLO1 = 30.0; sc.SLO2 = 15.0; sc.tp_UB = 0.5; sc.tp_base = 0.02; sc.dist_base = 2.0;
    sc.w_tp = 0.5; sc.w_c = 0.5;

    TaskTable table;
    table.N = 2;
    table.raw_rows = {
        {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
        {32, 3.0, 10.0, 2.0, 4.0, 12.0, 4.0}
    };

    int sim_to_solver[2];
    int solver_to_sim[2];

    if (pipe(sim_to_solver) < 0 || pipe(solver_to_sim) < 0) return;

    auto startTime = std::chrono::high_resolution_clock::now();

    pid_t pid = fork();
    if (pid == 0) {
        dup2(sim_to_solver[0], STDIN_FILENO);
        dup2(solver_to_sim[1], STDOUT_FILENO);
        close(sim_to_solver[0]); close(sim_to_solver[1]);
        close(solver_to_sim[0]); close(solver_to_sim[1]);

        execl("./solver", "./solver", "--adaptive", nullptr);
        exit(1);
    }

    close(sim_to_solver[0]);
    close(solver_to_sim[1]);

    FILE* out_to_solver = fdopen(sim_to_solver[1], "w");
    FILE* in_from_solver = fdopen(solver_to_sim[0], "r");

    fprintf(out_to_solver, "%d %.9f %.9f %.9f %lld %d\n", 
            sys.K, sys.S, sys.latency_in_ms, sys.bandwidth_gbps, sys.bytes_per_token, sys.num_layers);

    fprintf(out_to_solver, "%.9f %.9f %.9f %.9f %.9f %.9f %.9f\n",
            sc.SLO1, sc.SLO2, sc.tp_UB, sc.tp_base, sc.dist_base, sc.w_tp, sc.w_c);

    fprintf(out_to_solver, "%d\n", table.N);
    for (const auto& row : table.raw_rows) {
        fprintf(out_to_solver, "%d %.9f %.9f %.9f %.9f %.9f %.9f\n",
                row.batch_size, row.prefill_pre, row.prefill_proc, row.prefill_post,
                row.decode_pre, row.decode_proc, row.decode_post);
    }
    fflush(out_to_solver);

    double t = 0.0;
    int nextArr = 0;
    int finishedRequests = 0;

    struct ReqState {
        int rid;
        double arrTime;
        int tokensDone = 0;
        int assignedRemote = -1;
    };
    std::vector<ReqState> reqs(numRequests);
    for (int i = 0; i < numRequests; ++i) {
        reqs[i] = {i, i * 0.05, 0, -1};
    }

    std::vector<PerfPendingEv> pending;
    long long frameCount = 0;

    while (finishedRequests < numRequests) {
        frameCount++;
        std::vector<std::string> frameLines;

        while (nextArr < numRequests && reqs[nextArr].arrTime == t) {
            frameLines.push_back("ARR " + std::to_string(nextArr) + " 4");
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
            std::string server_str = serverbuf, step = stepbuf, sub = subbuf;

            if (step == "P" && sub == "PRE") {
                int remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d", &remote, &rid);
                reqs[rid].assignedRemote = remote;
                pending.push_back(PerfPendingEv{t + sys.S + 3.0, "TDN", "TDN " + server_str + " P PRE " + std::to_string(remote) + " " + std::to_string(rid) + " 3.000000000", rid});
                pending.push_back(PerfPendingEv{t + sys.S + 3.0 + 3.0, "XDN_UP", "XDN UP " + std::to_string(remote) + " 500000 PRE 1 " + std::to_string(rid), rid});

            } else if (step == "P" && sub == "PROC") {
                int ls = 0, le = 0, remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d %d %d", &ls, &le, &remote, &rid);
                pending.push_back(PerfPendingEv{t + sys.S + 10.0, "TDN", "TDN " + server_str + " P PROC 0 4 " + std::to_string(remote) + " " + std::to_string(rid) + " 10.000000000", rid});
                pending.push_back(PerfPendingEv{t + sys.S + 10.0 + 3.0, "XDN_DOWN", "XDN DOWN " + std::to_string(remote) + " 500000 PRE 1 " + std::to_string(rid), rid});

            } else if (step == "P" && sub == "POST") {
                int remote = 0, rid = 0;
                fscanf(in_from_solver, "%d %d", &remote, &rid);
                pending.push_back(PerfPendingEv{t + sys.S + 2.0, "TDN", "TDN " + server_str + " P POST " + std::to_string(remote) + " " + std::to_string(rid) + " 2.000000000", rid});

            } else if (step == "D" && sub == "PRE") {
                int dummy = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &dummy, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back(PerfPendingEv{t + sys.S + 1.5, "TDN", "TDN " + server_str + " D PRE -1 " + std::to_string(m) + ridsStr + " 1.500000000", rids[0]});
                pending.push_back(PerfPendingEv{t + sys.S + 1.5 + 3.0, "XDN_UP", "XDN UP 0 125000 DEC " + std::to_string(m) + ridsStr, rids[0]});

            } else if (step == "D" && sub == "PROC") {
                int remote = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &remote, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back(PerfPendingEv{t + sys.S + 6.0, "TDN", "TDN " + server_str + " D PROC " + std::to_string(remote) + " " + std::to_string(m) + ridsStr + " 6.000000000", rids[0]});
                pending.push_back(PerfPendingEv{t + sys.S + 6.0 + 3.0, "XDN_DOWN", "XDN DOWN " + std::to_string(remote) + " 125000 DEC " + std::to_string(m) + ridsStr, rids[0]});

            } else if (step == "D" && sub == "POST") {
                int dummy = 0, m = 0;
                fscanf(in_from_solver, "%d %d", &dummy, &m);
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) fscanf(in_from_solver, "%d", &rids[r]);
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back(PerfPendingEv{t + sys.S + 1.5, "TDN", "TDN " + server_str + " D POST -1 " + std::to_string(m) + ridsStr + " 1.500000000", rids[0]});

                for (int rid : rids) {
                    reqs[rid].tokensDone++;
                    if (reqs[rid].tokensDone == numTokens) {
                        finishedRequests++;
                        pending.push_back(PerfPendingEv{t + sys.S + 1.5, "FIN", "FIN " + std::to_string(rid), rid});
                    }
                }
            }
        }

        double nextT = 1e18;
        for (const auto& ev : pending) nextT = std::min(nextT, ev.time);
        if (nextArr < numRequests) nextT = std::min(nextT, reqs[nextArr].arrTime);

        if (nextT >= 1e17) break;
        t = nextT;
    }

    fprintf(out_to_solver, "END\n");
    fflush(out_to_solver);
    fclose(out_to_solver);
    fclose(in_from_solver);

    int status = 0;
    waitpid(pid, &status, 0);

    auto endTime = std::chrono::high_resolution_clock::now();
    double durationMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);
    double memoryMb = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);

    std::cout << std::left << std::setw(15) << numRequests
              << std::setw(15) << frameCount
              << std::setw(18) << std::fixed << std::setprecision(2) << durationMs
              << std::setw(15) << std::setprecision(3) << (durationMs / frameCount * 1000.0)
              << std::setprecision(2) << memoryMb << " MB\n";
}

int main() {
    std::cout << "=========================================================================\n";
    std::cout << "     HIGH-STRESS PERFORMANCE BENCHMARK (R=2000, 100K to 2M Frames)       \n";
    std::cout << "=========================================================================\n";
    std::cout << std::left << std::setw(15) << "Requests (R)"
              << std::setw(15) << "Total Frames"
              << std::setw(18) << "Elapsed Time (ms)"
              << std::setw(15) << "us/frame"
              << "Peak Memory (RSS)\n";
    std::cout << "-------------------------------------------------------------------------\n";

    runPerformanceBenchmark(100, 50);
    runPerformanceBenchmark(500, 100);
    runPerformanceBenchmark(1000, 200);
    runPerformanceBenchmark(2000, 500);

    std::cout << "=========================================================================\n";
    return 0;
}
