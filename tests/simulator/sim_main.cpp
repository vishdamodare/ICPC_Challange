#include "../../src/protocol.hpp"
#include "../../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <string>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <algorithm>

struct PendingTask {
    double finishTime;
    std::string server;
    std::string task_spec;
    double dur;
};

struct PendingTransfer {
    double finishTime;
    std::string direction; // "UP" or "DOWN"
    int remote;
    long long size;
    std::string stage_tag; // "PRE" or "DEC"
    int m;
    std::vector<int> rids;
};

struct SimReq {
    int rid;
    double arrivalTime;
    int Lin;
    int Lout;
    int tokensDone = 0;
    double ppostTime = 0.0;
    std::vector<double> tokenTimes;
};

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // Scenario setup
    SystemConfig sys;
    sys.K = 1;
    sys.S = 1.0;
    sys.latency_in_ms = 2.0;
    sys.bandwidth_gbps = 1.0;
    sys.bytes_per_token = 125000;
    sys.num_layers = 4;

    ScoringConfig sc;
    sc.SLO1 = 30.0;
    sc.SLO2 = 15.0;
    sc.tp_UB = 0.0625;
    sc.tp_base = 0.022222222;
    sc.dist_base = 0.0;
    sc.w_tp = 0.5;
    sc.w_c = 0.5;

    TaskTable table;
    table.N = 2;
    table.raw_rows = {
        {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
        {4, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}
    };

    std::vector<SimReq> requests = {
        {0, 0.0, 4, 1, 0, 0.0, {}}
    };

    // Print Config & Warmup Table to stdout (read by solver)
    std::cout << sys.K << " " << std::fixed << std::setprecision(9) 
              << sys.S << " " << sys.latency_in_ms << " " << sys.bandwidth_gbps << " " 
              << sys.bytes_per_token << " " << sys.num_layers << "\n";

    std::cout << sc.SLO1 << " " << sc.SLO2 << " " << sc.tp_UB << " " 
              << sc.tp_base << " " << sc.dist_base << " " << sc.w_tp << " " << sc.w_c << "\n";

    std::cout << table.N << "\n";
    for (const auto& row : table.raw_rows) {
        std::cout << row.batch_size << " " << row.prefill_pre << " " << row.prefill_proc << " " 
                  << row.prefill_post << " " << row.decode_pre << " " << row.decode_proc << " " 
                  << row.decode_post << "\n";
    }
    std::cout << std::flush;

    // Simulation state
    double currentTime = 0.0;
    std::vector<PendingTask> pendingTasks;
    std::vector<PendingTransfer> upQueue;
    std::vector<PendingTransfer> downQueue;

    double upAvailableTime = 0.0;
    double downAvailableTime = 0.0;

    int nextArrivalIdx = 0;
    int finishedCount = 0;
    int totalRequests = requests.size();

    // Event loop
    while (finishedCount < totalRequests) {
        // Collect events happening at currentTime
        std::vector<Event> frameEvents;

        // Check arrivals
        while (nextArrivalIdx < totalRequests && requests[nextArrivalIdx].arrivalTime == currentTime) {
            Event ev;
            ev.type = EventType::ARR;
            ev.rid = requests[nextArrivalIdx].rid;
            ev.Lin = requests[nextArrivalIdx].Lin;
            frameEvents.push_back(ev);
            nextArrivalIdx++;
        }

        // Check finished tasks
        for (auto it = pendingTasks.begin(); it != pendingTasks.end(); ) {
            if (it->finishTime == currentTime) {
                Event ev;
                ev.type = EventType::TDN;
                ev.server = it->server;
                ev.task_spec = it->task_spec;
                ev.dur = it->dur;
                frameEvents.push_back(ev);

                // Track P POST completion for TDR
                std::stringstream ss(it->task_spec);
                std::string p1, p2;
                ss >> p1 >> p2;
                if (p1 == "P" && p2 == "POST") {
                    int remote = 0, rid = 0;
                    ss >> remote >> rid;
                    requests[rid].ppostTime = currentTime;
                }

                it = pendingTasks.erase(it);
            } else {
                ++it;
            }
        }

        // Check finished UP transfers
        if (!upQueue.empty() && upQueue.front().finishTime == currentTime) {
            const auto& tr = upQueue.front();
            Event ev;
            ev.type = EventType::XDN;
            ev.direction = "UP";
            ev.remote = tr.remote;
            ev.size = tr.size;
            ev.stage_tag = tr.stage_tag;
            ev.m = tr.m;
            ev.rids = tr.rids;
            ev.rid = tr.rids.empty() ? -1 : tr.rids[0];
            frameEvents.push_back(ev);
            upQueue.erase(upQueue.begin());
        }

        // Check finished DOWN transfers
        if (!downQueue.empty() && downQueue.front().finishTime == currentTime) {
            const auto& tr = downQueue.front();
            Event ev;
            ev.type = EventType::XDN;
            ev.direction = "DOWN";
            ev.remote = tr.remote;
            ev.size = tr.size;
            ev.stage_tag = tr.stage_tag;
            ev.m = tr.m;
            ev.rids = tr.rids;
            ev.rid = tr.rids.empty() ? -1 : tr.rids[0];
            frameEvents.push_back(ev);
            downQueue.erase(downQueue.begin());
        }

        // Output frame to solver
        std::cout << currentTime << "\n" << frameEvents.size() << "\n";
        for (const auto& ev : frameEvents) {
            if (ev.type == EventType::ARR) {
                std::cout << "ARR " << ev.rid << " " << ev.Lin << "\n";
            } else if (ev.type == EventType::TDN) {
                std::cout << "TDN " << ev.server << " " << ev.task_spec << " " << ev.dur << "\n";
            } else if (ev.type == EventType::XDN) {
                std::cout << "XDN " << ev.direction << " " << ev.remote << " " << ev.size << " " << ev.stage_tag << " " << ev.m;
                for (int r : ev.rids) std::cout << " " << r;
                std::cout << "\n";
            } else if (ev.type == EventType::FIN) {
                std::cout << "FIN " << ev.rid << "\n";
            }
        }
        std::cout << std::flush;

        // Read solver response
        int n = 0;
        if (!(std::cin >> n)) break;
        std::string line;
        std::getline(std::cin, line); // consume remainder

        for (int i = 0; i < n; ++i) {
            std::getline(std::cin, line);
            std::stringstream ss(line);
            std::string server_str, p1, p2;
            ss >> server_str >> p1 >> p2;

            if (p1 == "P" && p2 == "PRE") {
                int remote = 0, rid = 0;
                ss >> remote >> rid;
                double dur = table.getDuration(TaskStep::PREFILL_PRE, requests[rid].Lin);
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

                // Queue UP transfer
                long long bytes = requests[rid].Lin * sys.bytes_per_token;
                double transferDur = sys.latency_in_ms + 8.0 * bytes / (sys.bandwidth_gbps * 1e6);
                double startTr = std::max(currentTime + sys.S + dur, upAvailableTime);
                double endTr = startTr + transferDur;
                upAvailableTime = endTr;
                upQueue.push_back({endTr, "UP", remote, bytes, "PRE", 1, {rid}});

            } else if (p1 == "P" && p2 == "PROC") {
                int ls = 0, le = 0, remote = 0, rid = 0;
                ss >> ls >> le >> remote >> rid;
                double baseDur = table.getDuration(TaskStep::PREFILL_PROC, requests[rid].Lin);
                double dur = (static_cast<double>(le - ls) / sys.num_layers) * baseDur;
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

                if (le == sys.num_layers) {
                    long long bytes = requests[rid].Lin * sys.bytes_per_token;
                    double transferDur = sys.latency_in_ms + 8.0 * bytes / (sys.bandwidth_gbps * 1e6);
                    double startTr = std::max(currentTime + sys.S + dur, downAvailableTime);
                    double endTr = startTr + transferDur;
                    downAvailableTime = endTr;
                    downQueue.push_back({endTr, "DOWN", remote, bytes, "PRE", 1, {rid}});
                }

            } else if (p1 == "P" && p2 == "POST") {
                int remote = 0, rid = 0;
                ss >> remote >> rid;
                double dur = table.getDuration(TaskStep::PREFILL_POST, requests[rid].Lin);
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

            } else if (p1 == "D" && p2 == "PRE") {
                int dummy = 0, m = 0;
                ss >> dummy >> m;
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) ss >> rids[r];
                double dur = table.getDuration(TaskStep::DECODE_PRE, m);
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

                // Group by remote
                std::map<int, std::vector<int>> perRemote;
                for (int rid : rids) {
                    // find assigned remote
                    // for test, rid=0 -> remote 0
                    perRemote[0].push_back(rid);
                }
                for (const auto& kv : perRemote) {
                    int remote = kv.first;
                    int sub_m = kv.second.size();
                    long long bytes = sub_m * sys.bytes_per_token;
                    double transferDur = sys.latency_in_ms + 8.0 * bytes / (sys.bandwidth_gbps * 1e6);
                    double startTr = std::max(currentTime + sys.S + dur, upAvailableTime);
                    double endTr = startTr + transferDur;
                    upAvailableTime = endTr;
                    upQueue.push_back({endTr, "UP", remote, bytes, "DEC", sub_m, kv.second});
                }

            } else if (p1 == "D" && p2 == "PROC") {
                int remote = 0, m = 0;
                ss >> remote >> m;
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) ss >> rids[r];
                double dur = table.getDuration(TaskStep::DECODE_PROC, m);
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

                long long bytes = m * sys.bytes_per_token;
                double transferDur = sys.latency_in_ms + 8.0 * bytes / (sys.bandwidth_gbps * 1e6);
                double startTr = std::max(currentTime + sys.S + dur, downAvailableTime);
                double endTr = startTr + transferDur;
                downAvailableTime = endTr;
                downQueue.push_back({endTr, "DOWN", remote, bytes, "DEC", m, rids});

            } else if (p1 == "D" && p2 == "POST") {
                int dummy = 0, m = 0;
                ss >> dummy >> m;
                std::vector<int> rids(m);
                for (int r = 0; r < m; ++r) ss >> rids[r];
                double dur = table.getDuration(TaskStep::DECODE_POST, m);
                pendingTasks.push_back({currentTime + sys.S + dur, server_str, line.substr(server_str.size() + 1), dur});

                for (int rid : rids) {
                    requests[rid].tokensDone++;
                    requests[rid].tokenTimes.push_back(currentTime + sys.S + dur);
                    if (requests[rid].tokensDone == requests[rid].Lout) {
                        finishedCount++;
                    }
                }
            }
        }

        // Find next event timestamp
        double nextTime = 1e18;
        for (const auto& t : pendingTasks) nextTime = std::min(nextTime, t.finishTime);
        if (!upQueue.empty()) nextTime = std::min(nextTime, upQueue.front().finishTime);
        if (!downQueue.empty()) nextTime = std::min(nextTime, downQueue.front().finishTime);
        if (nextArrivalIdx < totalRequests) nextTime = std::min(nextTime, requests[nextArrivalIdx].arrivalTime);

        if (nextTime >= 1e17) break;
        currentTime = nextTime;
    }

    std::cout << "END\n" << std::flush;
    return 0;
}
