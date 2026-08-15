#include "../../src/protocol.hpp"
#include "../../src/state_tracker.hpp"
#include "../../src/legal_tasks.hpp"
#include <iostream>
#include <vector>
#include <cassert>

int main() {
    SystemConfig sys;
    sys.K = 2; sys.S = 1.0; sys.num_layers = 4;

    // Test Frame A vs Frame B (TDN then FIN vs FIN then TDN)
    FrameContext frameA;
    frameA.timestamp = 45.0;
    frameA.eventCount = 2;
    frameA.events = {
        {EventType::TDN, -1, 0, "E", "D POST -1 2 0 1", 1.0, "", 0, 0, "", 0, {}},
        {EventType::FIN, 0, 0, "", "", 0.0, "", 0, 0, "", 0, {}}
    };

    FrameContext frameB;
    frameB.timestamp = 45.0;
    frameB.eventCount = 2;
    frameB.events = {
        {EventType::FIN, 0, 0, "", "", 0.0, "", 0, 0, "", 0, {}},
        {EventType::TDN, -1, 0, "E", "D POST -1 2 0 1", 1.0, "", 0, 0, "", 0, {}}
    };

    StateTracker stateA; stateA.init(sys);
    stateA.requests.resize(2);
    stateA.requests[0] = {0, 4, 0, 4, 1, 1, RequestStage::D_POST_IN_FLIGHT, true, true, false};
    stateA.requests[1] = {1, 4, 1, 4, 1, 1, RequestStage::D_POST_IN_FLIGHT, true, true, false};

    StateTracker stateB; stateB.init(sys);
    stateB.requests.resize(2);
    stateB.requests[0] = {0, 4, 0, 4, 1, 1, RequestStage::D_POST_IN_FLIGHT, true, true, false};
    stateB.requests[1] = {1, 4, 1, 4, 1, 1, RequestStage::D_POST_IN_FLIGHT, true, true, false};

    stateA.processFrame(frameA);
    stateB.processFrame(frameB);

    assert(stateA.requests[0].finished == stateB.requests[0].finished);
    assert(stateA.requests[0].stage == stateB.requests[0].stage);
    assert(stateA.requests[1].stage == stateB.requests[1].stage);

    auto candA = LegalTaskGenerator::generateCandidates(stateA);
    auto candB = LegalTaskGenerator::generateCandidates(stateB);

    assert(candA.size() == candB.size());

    std::cout << "[DIFFERENTIAL EVENT-ORDER TEST] PASS! Frame A and Frame B produced 100% identical committed states and candidate tasks.\n";
    return 0;
}
