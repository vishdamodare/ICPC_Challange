#include "../src/protocol.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/adaptive_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sys/resource.h>

void runFrameCountStressTest(long long targetFrames) {
    SystemConfig sys;
    sys.K = 8; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    TaskTable table;
    table.N = 2;
    table.raw_rows = {
        {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
        {32, 3.0, 10.0, 2.0, 4.0, 12.0, 4.0}
    };

    StateTracker state;
    state.init(sys);
    AdaptiveStrategy strategy(table);

    double stateUpdateTimeMs = 0.0;
    double candidateGenTimeMs = 0.0;
    double strategyTimeMs = 0.0;
    double conflictTimeMs = 0.0;

    auto startOverall = std::chrono::high_resolution_clock::now();

    for (long long f = 0; f < targetFrames; ++f) {
        FrameContext frame;
        frame.timestamp = f * 0.1;
        frame.eventCount = 1;

        // Alternate frame event types under saturated 2000 request pool
        int rid = f % 2000;
        if (f % 4 == 0) {
            frame.events = {{EventType::ARR, rid, 4, "", "", 0.0, "", 0, 0, "", 0, {}}};
        } else if (f % 4 == 1) {
            frame.events = {{EventType::TDN, -1, 0, "E", "D POST -1 1 " + std::to_string(rid), 1.0, "", 0, 0, "", 0, {}}};
        } else if (f % 4 == 2) {
            frame.events = {{EventType::XDN, -1, 0, "", "", 0.0, "UP", 0, 125000, "DEC", 1, {rid}}};
        } else {
            frame.events = {{EventType::FIN, rid, 0, "", "", 0.0, "", 0, 0, "", 0, {}}};
        }

        // 1. State Update
        auto t0 = std::chrono::high_resolution_clock::now();
        state.processFrame(frame);
        auto t1 = std::chrono::high_resolution_clock::now();
        stateUpdateTimeMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 2. Candidate Generation
        auto candidates = LegalTaskGenerator::generateCandidates(state);
        auto t2 = std::chrono::high_resolution_clock::now();
        candidateGenTimeMs += std::chrono::duration<double, std::milli>(t2 - t1).count();

        // 3. Strategy Selection
        auto selected = strategy.selectTasks(state, candidates);
        auto t3 = std::chrono::high_resolution_clock::now();
        strategyTimeMs += std::chrono::duration<double, std::milli>(t3 - t2).count();

        // 4. Conflict Resolution
        auto valid = ConflictResolver::resolveConflicts(state, selected);
        auto t4 = std::chrono::high_resolution_clock::now();
        conflictTimeMs += std::chrono::duration<double, std::milli>(t4 - t3).count();

        for (const auto& task : valid) {
            state.markTaskAssigned(task);
        }
    }

    auto endOverall = std::chrono::high_resolution_clock::now();
    double totalTimeMs = std::chrono::duration<double, std::milli>(endOverall - startOverall).count();

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double memoryMb = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);

    std::cout << std::left << std::setw(15) << targetFrames
              << std::setw(18) << std::fixed << std::setprecision(2) << totalTimeMs
              << std::setw(15) << std::setprecision(3) << (totalTimeMs / targetFrames * 1000.0)
              << std::setw(15) << (stateUpdateTimeMs / totalTimeMs * 100.0)
              << std::setw(15) << (candidateGenTimeMs / totalTimeMs * 100.0)
              << std::setw(15) << (strategyTimeMs / totalTimeMs * 100.0)
              << std::setprecision(2) << memoryMb << " MB\n";
}

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     TRUE 2,000,000 EVENT FRAME HIGH-STRESS BENCHMARK (O(ready) Profiling)               \n";
    std::cout << "=========================================================================================\n";
    std::cout << std::left << std::setw(15) << "Target Frames"
              << std::setw(18) << "Total CPU (ms)"
              << std::setw(15) << "us/frame"
              << std::setw(15) << "% StateUpdate"
              << std::setw(15) << "% CandGen"
              << std::setw(15) << "% Strategy"
              << "Peak RSS Memory\n";
    std::cout << "-----------------------------------------------------------------------------------------\n";

    runFrameCountStressTest(100000);
    runFrameCountStressTest(500000);
    runFrameCountStressTest(1000000);
    runFrameCountStressTest(2000000);

    std::cout << "=========================================================================================\n";
    return 0;
}
