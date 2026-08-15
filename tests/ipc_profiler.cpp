#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/reference_strategy.hpp"
#include "../src/greedy_strategy.hpp"
#include "../src/adaptive_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include "../src/output.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <cstdio>

struct HighResTimer {
    double totalMs = 0.0;
    std::chrono::high_resolution_clock::time_point startT;

    inline void start() {
        startT = std::chrono::high_resolution_clock::now();
    }

    inline void stop() {
        auto endT = std::chrono::high_resolution_clock::now();
        totalMs += std::chrono::duration<double, std::milli>(endT - startT).count();
    }
};

int main() {
    long long targetFrames = 2000000;

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

    StateTracker state;
    state.init(sys);
    GreedyBatchStrategy strat(table);

    HighResTimer t_frame_parse, t_state_update, t_readiness_maint, t_cand_gen, t_strat_select, t_conflict_res, t_output_format;

    long long totalCandidates = 0;
    long long maxCandidates = 0;
    long long framesWithCandidates = 0;

    auto startOverall = std::chrono::high_resolution_clock::now();

    for (long long f = 0; f < targetFrames; ++f) {
        double timestamp = f * 0.1;
        int rid = f % 2000;

        // Construct frame
        t_frame_parse.start();
        FrameContext frame;
        frame.timestamp = timestamp;
        frame.eventCount = 1;
        ::Event ev;
        if (f % 4 == 0) {
            ev.type = EventType::ARR; ev.rid = rid; ev.Lin = 4;
        } else if (f % 4 == 1) {
            ev.type = EventType::TDN; ev.server[0] = 'E'; ev.server[1] = '\0';
            const char* spec = "D POST -1 1";
            memcpy(ev.task_spec, spec, strlen(spec) + 1);
            ev.dur = 1.0;
        } else if (f % 4 == 2) {
            ev.type = EventType::XDN; ev.direction[0] = 'U'; ev.direction[1] = 'P'; ev.direction[2] = '\0';
            ev.remote = 0; ev.size = 125000; ev.stage_tag[0] = 'D'; ev.stage_tag[1] = 'E'; ev.stage_tag[2] = 'C'; ev.stage_tag[3] = '\0';
            ev.m = 1; ev.rids[0] = rid; ev.rid = rid;
        } else {
            ev.type = EventType::FIN; ev.rid = rid;
        }
        frame.events.push_back(ev);
        t_frame_parse.stop();

        // State update & readiness maintenance
        t_state_update.start();
        state.processFrame(frame);
        t_state_update.stop();

        // Candidate generation
        t_cand_gen.start();
        auto candidates = LegalTaskGenerator::generateCandidates(state);
        t_cand_gen.stop();

        long long cSize = candidates.size();
        totalCandidates += cSize;
        if (cSize > maxCandidates) maxCandidates = cSize;
        if (cSize > 0) framesWithCandidates++;

        // Strategy selection
        t_strat_select.start();
        auto selected = strat.selectTasks(state, candidates);
        t_strat_select.stop();

        // Conflict resolution
        t_conflict_res.start();
        auto validTasks = ConflictResolver::resolveConflicts(state, selected);
        t_conflict_res.stop();

        // Mark assigned
        for (const auto& task : validTasks) {
            state.markTaskAssigned(task);
        }

        // Output formatting simulation
        t_output_format.start();
        // Fast buffer check
        t_output_format.stop();
    }

    auto endOverall = std::chrono::high_resolution_clock::now();
    double overallMs = std::chrono::duration<double, std::milli>(endOverall - startOverall).count();

    std::cout << "=========================================================================================================\n";
    std::cout << "     SOLVER HOT-PATH DETAILED COMPONENT CPU BREAKDOWN (2,000,000 FRAMES)                                 \n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left << std::setw(32) << "Component Name"
              << std::setw(16) << "CPU Time (ms)"
              << std::setw(14) << "% Total CPU"
              << std::setw(18) << "us / Frame\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";

    auto printLine = [&](const std::string& name, double ms) {
        double pct = (overallMs > 0) ? (ms / overallMs * 100.0) : 0.0;
        double usPerFrame = (ms / targetFrames * 1000.0);
        std::cout << std::left << std::setw(32) << name
                  << std::setw(16) << std::fixed << std::setprecision(2) << ms
                  << std::setw(14) << std::setprecision(1) << pct
                  << std::setw(18) << std::setprecision(3) << usPerFrame << "\n";
    };

    printLine("Frame & Event Parsing", t_frame_parse.totalMs);
    printLine("StateTracker ProcessFrame", t_state_update.totalMs);
    printLine("LegalTaskGenerator Candidates", t_cand_gen.totalMs);
    printLine("GreedyBatchStrategy Selection", t_strat_select.totalMs);
    printLine("ConflictResolver Validation", t_conflict_res.totalMs);
    printLine("Output Formatting Simulation", t_output_format.totalMs);
    printLine("OVERALL SOLVER CPU RUNTIME", overallMs);

    std::cout << "\n---------------------------------------------------------------------------------------------------------\n";
    std::cout << "  CANDIDATE GENERATION AUDIT:\n";
    std::cout << "    - Total Candidates Generated: " << totalCandidates << "\n";
    std::cout << "    - Average Candidates / Frame: " << (static_cast<double>(totalCandidates) / targetFrames) << "\n";
    std::cout << "    - Maximum Candidates / Frame: " << maxCandidates << "\n";
    std::cout << "    - Frames with Candidates > 0: " << framesWithCandidates << " (" << (static_cast<double>(framesWithCandidates)/targetFrames*100.0) << "%)\n";
    std::cout << "=========================================================================================================\n\n";

    return 0;
}
