#include "protocol.hpp"
#include "task_table.hpp"
#include "state_tracker.hpp"
#include "legal_tasks.hpp"
#include "reference_strategy.hpp"
#include "greedy_strategy.hpp"
#include "adaptive_strategy.hpp"
#include "conflict_resolver.hpp"
#include "output.hpp"
#include <iostream>
#include <memory>
#include <cstdio>

int main(int argc, char** argv) {
    // Enable 64KB C stdio buffering for maximum stream throughput & zero desync risk
    setvbuf(stdin, nullptr, _IOFBF, 65536);
    setvbuf(stdout, nullptr, _IOFBF, 65536);

    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    SystemConfig sys = InteractiveIO::parseSystemConfig(std::cin);
    ScoringConfig sc = InteractiveIO::parseScoringConfig(std::cin);

    TaskTable taskTable;
    taskTable.parse(std::cin);

    StateTracker state;
    state.init(sys);

    std::unique_ptr<SchedulingStrategy> strategy;
    if (argc > 1 && std::string(argv[1]) == "--ref") {
        strategy = std::make_unique<ReferenceStrategy>();
    } else if (argc > 1 && std::string(argv[1]) == "--adaptive") {
        strategy = std::make_unique<AdaptiveStrategy>(taskTable);
    } else {
        strategy = std::make_unique<GreedyBatchStrategy>(taskTable);
    }

    while (true) {
        FrameContext frame = InteractiveIO::parseFrame(std::cin);
        if (frame.isEnd) {
            break;
        }

        // Apply ALL events in the frame atomically
        state.processFrame(frame);

        // Generate legal task candidates
        auto candidates = LegalTaskGenerator::generateCandidates(state);

        // Select task assignments using current strategy
        auto selected = strategy->selectTasks(state, candidates);

        // Validate conflicts (resource, request, same-response safety)
        auto validTasks = ConflictResolver::resolveConflicts(state, selected);

        // Mark assigned tasks as in-flight in StateTracker
        for (const auto& task : validTasks) {
            state.markTaskAssigned(task);
        }

        // Emit assignments to interactor & flush
        OutputWriter::writeAssignments(std::cout, validTasks);
    }

    return 0;
}
