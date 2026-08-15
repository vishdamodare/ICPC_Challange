#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/greedy_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include "../src/output.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <sstream>
#include <string>
#include <cstring>

static void runParanoiaTestCase1_LargeK() {
    std::cout << "  [PARANOIA 1] Testing Large K (K=32, Remote=31) ... ";
    SystemConfig sys;
    sys.K = 32; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    TaskTable table;
    table.N = 1;
    table.raw_rows = {{1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}};

    StateTracker state;
    state.init(sys);
    GreedyBatchStrategy strat(table);

    // Frame 1: ARR for rid 0
    FrameContext f1;
    f1.timestamp = 0.0;
    f1.eventCount = 1;
    Event ev1; ev1.type = EventType::ARR; ev1.rid = 0; ev1.Lin = 4;
    f1.events = {ev1};

    state.processFrame(f1);
    auto candidates = LegalTaskGenerator::generateCandidates(state);
    auto selected = strat.selectTasks(state, candidates);
    auto valid = ConflictResolver::resolveConflicts(state, selected);

    assert(!valid.empty());
    assert(valid[0].type == TaskType::P_PRE);
    assert(valid[0].server == -1);
    state.markTaskAssigned(valid[0]);

    // Frame 2: TDN E P PRE 31 0
    FrameContext f2;
    f2.timestamp = 4.0;
    f2.eventCount = 1;
    Event ev2; ev2.type = EventType::TDN;
    strcpy(ev2.server, "E");
    ev2.task_spec = "P PRE 31 0";
    ev2.dur = 4.0;
    f2.events = {ev2};

    state.processFrame(f2);
    assert(state.requests[0].assignedRemote == 31);
    assert(state.requests[0].stage == RequestStage::P_WAIT_UP);

    std::cout << "[PASS]\n";
}

static void runParanoiaTestCase2_LargeBatch() {
    std::cout << "  [PARANOIA 2] Testing Large Batch Size (m=256, m=1024) ... ";
    SystemConfig sys;
    sys.K = 4; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    TaskTable table;
    table.N = 1;
    table.raw_rows = {{1024, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0}};

    StateTracker state;
    state.init(sys);

    // XDN DEC DOWN event with m=256
    FrameContext f1;
    f1.timestamp = 10.0;
    f1.eventCount = 1;
    Event ev1; ev1.type = EventType::XDN;
    strcpy(ev1.direction, "DOWN");
    ev1.remote = 0;
    ev1.size = 256 * 125000;
    strcpy(ev1.stage_tag, "DEC");
    ev1.m = 256;
    ev1.rids.resize(256);
    for (int i = 0; i < 256; ++i) {
        ev1.rids[i] = i;
        if (i >= static_cast<int>(state.requests.size())) {
            state.requests.resize(i + 1);
        }
        state.requests[i].rid = i;
        state.requests[i].assignedRemote = 0;
        state.requests[i].stage = RequestStage::D_WAIT_DOWN;
    }
    f1.events = {ev1};

    state.processFrame(f1);

    // Verify ALL 256 requests were transitioned to D_POST_READY
    assert(static_cast<int>(state.dPostReadyList.size()) == 256);
    for (int i = 0; i < 256; ++i) {
        assert(state.requests[i].stage == RequestStage::D_POST_READY);
        assert(state.requests[i].decodeDownReady);
    }

    std::cout << "[PASS]\n";
}

static void runParanoiaTestCase3_CoalescedFrame() {
    std::cout << "  [PARANOIA 3] Testing Coalesced Same-Timestamp Events ... ";
    SystemConfig sys;
    sys.K = 2; sys.S = 1.0; sys.num_layers = 2;
    sys.latency_in_ms = 1.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 100;

    TaskTable table;
    table.N = 1;
    table.raw_rows = {{1, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}};

    StateTracker state;
    state.init(sys);

    // Coalesced frame: ARR 0, ARR 1, TDN E P PRE 0 0, FIN 99
    FrameContext f;
    f.timestamp = 5.0;
    f.eventCount = 3;

    Event e1; e1.type = EventType::ARR; e1.rid = 0; e1.Lin = 2;
    Event e2; e2.type = EventType::ARR; e2.rid = 1; e2.Lin = 2;
    Event e3; e3.type = EventType::FIN; e3.rid = 99;

    state.requests.resize(100);
    state.requests[99].rid = 99;
    state.requests[99].stage = RequestStage::D_POST_READY;
    state.dPostReadyList.push_back(99);

    f.events = {e1, e2, e3};
    state.processFrame(f);

    assert(state.requests[99].finished);
    assert(state.requests[99].stage == RequestStage::FINISHED);
    assert(state.dPostReadyList.empty());
    assert(state.pPreReadyList.size() == 2);

    std::cout << "[PASS]\n";
}

static void runParanoiaTestCase4_HighRequestID() {
    std::cout << "  [PARANOIA 4] Testing High Request ID (rid=9999) ... ";
    SystemConfig sys;
    sys.K = 4; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    StateTracker state;
    state.init(sys);

    FrameContext f;
    f.timestamp = 1.0;
    f.eventCount = 1;
    Event ev; ev.type = EventType::ARR; ev.rid = 9999; ev.Lin = 10;
    f.events = {ev};

    state.processFrame(f);
    assert(static_cast<int>(state.requests.size()) == 10000);
    assert(state.requests[9999].rid == 9999);
    assert(state.pPreReadyList.size() == 1);
    assert(state.pPreReadyList[0] == 9999);

    std::cout << "[PASS]\n";
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "     CODEFORCES JUDGE PARANOIA STRESS SUITE          \n";
    std::cout << "=====================================================\n";

    runParanoiaTestCase1_LargeK();
    runParanoiaTestCase2_LargeBatch();
    runParanoiaTestCase3_CoalescedFrame();
    runParanoiaTestCase4_HighRequestID();

    std::cout << "=====================================================\n";
    std::cout << "       ALL PARANOIA TEST CASES PASSED CLEANLY        \n";
    std::cout << "=====================================================\n";

    return 0;
}
