#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/reference_strategy.hpp"
#include "../src/greedy_strategy.hpp"
#include "../src/adaptive_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include "scenario_generator.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

std::string taskToString(const Task& t) {
    std::string s;
    if (t.server == -1) s += "E: ";
    else s += "C" + std::to_string(t.server) + ": ";

    switch (t.type) {
        case TaskType::P_PRE: s += "P_PRE remote=" + std::to_string(t.remote) + " rid=" + std::to_string(t.requests[0]); break;
        case TaskType::P_PROC: s += "P_PROC [" + std::to_string(t.ls) + ".." + std::to_string(t.le) + "] remote=" + std::to_string(t.remote) + " rid=" + std::to_string(t.requests[0]); break;
        case TaskType::P_POST: s += "P_POST remote=" + std::to_string(t.remote) + " rid=" + std::to_string(t.requests[0]); break;
        case TaskType::D_PRE: s += "D_PRE m=" + std::to_string(t.m) + " rids=["; for (int r : t.requests) s += std::to_string(r) + " "; s += "]"; break;
        case TaskType::D_PROC: s += "D_PROC remote=" + std::to_string(t.remote) + " m=" + std::to_string(t.m) + " rids=["; for (int r : t.requests) s += std::to_string(r) + " "; s += "]"; break;
        case TaskType::D_POST: s += "D_POST m=" + std::to_string(t.m) + " rids=["; for (int r : t.requests) s += std::to_string(r) + " "; s += "]"; break;
    }
    return s;
}

void logStrategyTrajectory(const std::string& name, SchedulingStrategy* strat, const Scenario& sc) {
    std::cout << "\n=====================================================================\n";
    std::cout << "  SCHEDULING DECISION TRAJECTORY LOG: " << name << "\n";
    std::cout << "=====================================================================\n";

    StateTracker state;
    state.init(sc.sys);

    // Create a multi-request burst arrival frame
    FrameContext frame;
    frame.timestamp = 0.0;
    frame.eventCount = 4;
    for (int i = 0; i < 4; ++i) {
        ::Event ev;
        ev.type = EventType::ARR;
        ev.rid = i;
        ev.Lin = 100;
        frame.events.push_back(ev);
    }

    state.processFrame(frame);
    auto candidates = LegalTaskGenerator::generateCandidates(state);
    auto selected = strat->selectTasks(state, candidates);

    std::cout << "  Frame t=0.0 (4 ARR arrivals):\n";
    std::cout << "    Candidate Count: " << candidates.size() << "\n";
    std::cout << "    Selected Assignments (" << selected.size() << "):\n";
    for (const auto& t : selected) {
        std::cout << "      - " << taskToString(t) << "\n";
    }

    // Advance state to D_PRE ready stage with 4 requests ready for decode
    FrameContext frameDec;
    frameDec.timestamp = 50.0;
    frameDec.eventCount = 4;
    for (int i = 0; i < 4; ++i) {
        ::Event ev;
        ev.type = EventType::TDN;
        ev.server[0] = 'E'; ev.server[1] = '\0';
        std::string spec = "P POST 0 " + std::to_string(i);
        memcpy(ev.task_spec, spec.c_str(), spec.length() + 1);
        frameDec.events.push_back(ev);
    }

    state.processFrame(frameDec);
    auto decCandidates = LegalTaskGenerator::generateCandidates(state);
    auto decSelected = strat->selectTasks(state, decCandidates);

    std::cout << "\n  Frame t=50.0 (4 requests D_PRE READY):\n";
    std::cout << "    Candidate Count: " << decCandidates.size() << "\n";
    std::cout << "    Selected Assignments (" << decSelected.size() << "):\n";
    for (const auto& t : decSelected) {
        std::cout << "      - " << taskToString(t) << "\n";
    }
}

int main() {
    Scenario sc = ScenarioGenerator::generateScenario(42);

    ReferenceStrategy v1;
    GreedyBatchStrategy v2(sc.table);
    AdaptiveStrategy v3(sc.table);

    logStrategyTrajectory("V1 Reference Strategy", &v1, sc);
    logStrategyTrajectory("V2 Greedy Batching Strategy", &v2, sc);
    logStrategyTrajectory("V3 Adaptive Strategy", &v3, sc);

    std::cout << "\n=====================================================================\n\n";
    return 0;
}
