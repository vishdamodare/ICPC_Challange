#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>

struct DecisionStats {
    int totalFrames = 0;
    int identicalFrames = 0;
    int dpreDiffs = 0;
    int dprocDiffs = 0;
    int dpostDiffs = 0;
    int ppreDiffs = 0;
    int batchSizeDiffs = 0;
};

DecisionStats compareDecisions(const std::vector<std::pair<double, int>>& arrivals, int Lout) {
    DecisionStats stats;

    SystemConfig sys;
    sys.K = 4; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    ScoringConfig sc;
    sc.SLO1 = 30.0; sc.SLO2 = 15.0; sc.tp_UB = 0.20; sc.tp_base = 0.02; sc.dist_base = 2.0;
    sc.w_tp = 0.5; sc.w_c = 0.5;

    TaskTable table;
    table.N = 4;
    table.raw_rows = {
        {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
        {4, 3.0, 10.0, 2.0, 1.5, 6.0, 1.5},
        {8, 3.0, 10.0, 2.0, 2.0, 8.0, 2.0},
        {20, 3.0, 10.0, 2.0, 3.0, 12.0, 3.0}
    };

    // Run V2
    int p2_v2_to_sim[2], p2_sim_to_v2[2];
    pipe(p2_v2_to_sim); pipe(p2_sim_to_v2);

    pid_t pid_v2 = fork();
    if (pid_v2 == 0) {
        dup2(p2_sim_to_v2[0], STDIN_FILENO); dup2(p2_v2_to_sim[1], STDOUT_FILENO);
        close(p2_v2_to_sim[0]); close(p2_v2_to_sim[1]); close(p2_sim_to_v2[0]); close(p2_sim_to_v2[1]);
        execl("./solver", "./solver", "--greedy", nullptr);
        exit(1);
    }
    close(p2_sim_to_v2[0]); close(p2_v2_to_sim[1]);
    FILE* out_v2 = fdopen(p2_sim_to_v2[1], "w");
    FILE* in_v2 = fdopen(p2_v2_to_sim[0], "r");

    // Run V3
    int p3_v3_to_sim[2], p3_sim_to_v3[2];
    pipe(p3_v3_to_sim); pipe(p3_sim_to_v3);

    pid_t pid_v3 = fork();
    if (pid_v3 == 0) {
        dup2(p3_sim_to_v3[0], STDIN_FILENO); dup2(p3_v3_to_sim[1], STDOUT_FILENO);
        close(p3_v3_to_sim[0]); close(p3_v3_to_sim[1]); close(p3_sim_to_v3[0]); close(p3_sim_to_v3[1]);
        execl("./solver", "./solver", "--adaptive", nullptr);
        exit(1);
    }
    close(p3_sim_to_v3[0]); close(p3_v3_to_sim[1]);
    FILE* out_v3 = fdopen(p3_sim_to_v3[1], "w");
    FILE* in_v3 = fdopen(p3_v3_to_sim[0], "r");

    // Send startup to both
    auto sendStartup = [&](FILE* f) {
        fprintf(f, "%d %.9f %.9f %.9f %lld %d\n", sys.K, sys.S, sys.latency_in_ms, sys.bandwidth_gbps, sys.bytes_per_token, sys.num_layers);
        fprintf(f, "%.9f %.9f %.9f %.9f %.9f %.9f %.9f\n", sc.SLO1, sc.SLO2, sc.tp_UB, sc.tp_base, sc.dist_base, sc.w_tp, sc.w_c);
        fprintf(f, "%d\n", table.N);
        for (const auto& row : table.raw_rows) {
            fprintf(f, "%d %.9f %.9f %.9f %.9f %.9f %.9f\n", row.batch_size, row.prefill_pre, row.prefill_proc, row.prefill_post, row.decode_pre, row.decode_proc, row.decode_post);
        }
        fflush(f);
    };

    sendStartup(out_v2);
    sendStartup(out_v3);

    // Synchronized simulation execution
    double t = 0.0;
    int nextArr = 0;
    int totalReqs = arrivals.size();
    int finishedRequests = 0;

    struct SimState {
        int rid;
        double arrTime;
        int Lin;
        int Lout;
        int tokensDone = 0;
    };
    std::map<int, SimState> reqs;

    struct PendingEv {
        double time;
        std::string payload;
        int rid;
    };
    std::vector<PendingEv> pending;

    while (finishedRequests < totalReqs) {
        stats.totalFrames++;

        std::vector<std::string> frameLines;
        while (nextArr < totalReqs && arrivals[nextArr].first == t) {
            reqs[nextArr] = {nextArr, t, arrivals[nextArr].second, Lout, 0};
            frameLines.push_back("ARR " + std::to_string(nextArr) + " " + std::to_string(arrivals[nextArr].second));
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

        auto sendFrame = [&](FILE* f) {
            fprintf(f, "%.9f\n%zu\n", t, frameLines.size());
            for (const auto& l : frameLines) fprintf(f, "%s\n", l.c_str());
            fflush(f);
        };

        sendFrame(out_v2);
        sendFrame(out_v3);

        // Read V2 tasks
        int n_v2 = 0; fscanf(in_v2, "%d", &n_v2);
        std::vector<std::string> tasks_v2(n_v2);
        for (int i = 0; i < n_v2; ++i) {
            char linebuf[256];
            fscanf(in_v2, " %[^\n]", linebuf);
            tasks_v2[i] = linebuf;
        }

        // Read V3 tasks
        int n_v3 = 0; fscanf(in_v3, "%d", &n_v3);
        std::vector<std::string> tasks_v3(n_v3);
        for (int i = 0; i < n_v3; ++i) {
            char linebuf[256];
            fscanf(in_v3, " %[^\n]", linebuf);
            tasks_v3[i] = linebuf;
        }

        // Compare decision vectors
        if (tasks_v2 == tasks_v3) {
            stats.identicalFrames++;
        } else {
            for (const auto& tv2 : tasks_v2) {
                if (tv2.find("D PRE") != std::string::npos) stats.dpreDiffs++;
                else if (tv2.find("D PROC") != std::string::npos) stats.dprocDiffs++;
                else if (tv2.find("D POST") != std::string::npos) stats.dpostDiffs++;
                else if (tv2.find("P PRE") != std::string::npos) stats.ppreDiffs++;
            }
        }

        for (const auto& line : tasks_v2) {
            std::stringstream ss(line);
            std::string server_str, p1, p2;
            ss >> server_str >> p1 >> p2;
            if (p1 == "P" && p2 == "PRE") {
                int remote = 0, rid = 0; ss >> remote >> rid;
                double dur = table.getDuration(TaskStep::PREFILL_PRE, reqs[rid].Lin);
                pending.push_back({t + sys.S + dur, "TDN " + server_str + " P PRE " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid});
                pending.push_back({t + sys.S + dur + 3.0, "XDN UP " + std::to_string(remote) + " 500000 PRE 1 " + std::to_string(rid), rid});
            } else if (p1 == "P" && p2 == "PROC") {
                int ls = 0, le = 0, remote = 0, rid = 0; ss >> ls >> le >> remote >> rid;
                double dur = table.getDuration(TaskStep::PREFILL_PROC, reqs[rid].Lin);
                pending.push_back({t + sys.S + dur, "TDN " + server_str + " P PROC 0 4 " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid});
                pending.push_back({t + sys.S + dur + 3.0, "XDN DOWN " + std::to_string(remote) + " 500000 PRE 1 " + std::to_string(rid), rid});
            } else if (p1 == "P" && p2 == "POST") {
                int remote = 0, rid = 0; ss >> remote >> rid;
                double dur = table.getDuration(TaskStep::PREFILL_POST, reqs[rid].Lin);
                pending.push_back({t + sys.S + dur, "TDN " + server_str + " P POST " + std::to_string(remote) + " " + std::to_string(rid) + " " + std::to_string(dur), rid});
            } else if (p1 == "D" && p2 == "PRE") {
                int dummy = 0, m = 0; ss >> dummy >> m;
                std::vector<int> rids(m); for (int r = 0; r < m; ++r) ss >> rids[r];
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back({t + sys.S + 1.5, "TDN " + server_str + " D PRE -1 " + std::to_string(m) + ridsStr + " 1.500000000", rids[0]});
                pending.push_back({t + sys.S + 1.5 + 3.0, "XDN UP 0 125000 DEC " + std::to_string(m) + ridsStr, rids[0]});
            } else if (p1 == "D" && p2 == "PROC") {
                int remote = 0, m = 0; ss >> remote >> m;
                std::vector<int> rids(m); for (int r = 0; r < m; ++r) ss >> rids[r];
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back({t + sys.S + 6.0, "TDN " + server_str + " D PROC " + std::to_string(remote) + " " + std::to_string(m) + ridsStr + " 6.000000000", rids[0]});
                pending.push_back({t + sys.S + 6.0 + 3.0, "XDN DOWN " + std::to_string(remote) + " 125000 DEC " + std::to_string(m) + ridsStr, rids[0]});
            } else if (p1 == "D" && p2 == "POST") {
                int dummy = 0, m = 0; ss >> dummy >> m;
                std::vector<int> rids(m); for (int r = 0; r < m; ++r) ss >> rids[r];
                std::string ridsStr; for (int r : rids) ridsStr += " " + std::to_string(r);
                pending.push_back({t + sys.S + 1.5, "TDN " + server_str + " D POST -1 " + std::to_string(m) + ridsStr + " 1.500000000", rids[0]});

                for (int rid : rids) {
                    reqs[rid].tokensDone++;
                    if (reqs[rid].tokensDone == Lout) {
                        finishedRequests++;
                        pending.push_back({t + sys.S + 1.5, "FIN " + std::to_string(rid), rid});
                    }
                }
            }
        }

        double nextT = 1e18;
        for (const auto& ev : pending) nextT = std::min(nextT, ev.time);
        if (nextArr < totalReqs) nextT = std::min(nextT, arrivals[nextArr].first);
        if (nextT >= 1e17) break;
        t = nextT;
    }

    fprintf(out_v2, "END\n"); fflush(out_v2); fclose(out_v2); fclose(in_v2);
    fprintf(out_v3, "END\n"); fflush(out_v3); fclose(out_v3); fclose(in_v3);

    int st1 = 0, st2 = 0;
    waitpid(pid_v2, &st1, 0);
    waitpid(pid_v3, &st2, 0);

    return stats;
}

int main() {
    std::cout << "=========================================================================\n";
    std::cout << "        DECISION DIFFERENCE REPORT: V2 (Greedy) vs V3 (Adaptive)         \n";
    std::cout << "=========================================================================\n";

    std::vector<std::pair<double, int>> arrivals;
    for (int i = 0; i < 20; ++i) arrivals.push_back({i * 0.2, 4});

    auto stats = compareDecisions(arrivals, 8);

    std::cout << std::left << std::setw(30) << "Total Decision Frames:" << stats.totalFrames << "\n";
    std::cout << std::left << std::setw(30) << "Identical Frames:" << stats.identicalFrames << " (" 
              << std::fixed << std::setprecision(1) << (100.0 * stats.identicalFrames / stats.totalFrames) << "% match)\n";
    std::cout << std::left << std::setw(30) << "D_PRE Differences:" << stats.dpreDiffs << "\n";
    std::cout << std::left << std::setw(30) << "D_PROC Differences:" << stats.dprocDiffs << "\n";
    std::cout << std::left << std::setw(30) << "D_POST Differences:" << stats.dpostDiffs << "\n";
    std::cout << std::left << std::setw(30) << "P_PRE Differences:" << stats.ppreDiffs << "\n";
    std::cout << "=========================================================================\n";

    return 0;
}
