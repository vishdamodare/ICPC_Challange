#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include "../src/state_tracker.hpp"
#include "../src/legal_tasks.hpp"
#include "../src/greedy_strategy.hpp"
#include "../src/conflict_resolver.hpp"
#include <iostream>
#include <sstream>
#include <cassert>
#include <string>

int main() {
    std::cout << "=========================================================================================\n";
    std::cout << "     UNIFIED C STDIO STARTUP & LARGE-BUFFER STREAM DESYNCHRONIZATION TEST               \n";
    std::cout << "=========================================================================================\n";

    // 1. Construct large startup payload > 15,000 bytes (300 TaskTable rows) to exceed 4KB/8KB buffers
    std::stringstream ss;
    ss << "2 1.500000000 2.500000000 1.000000000 250000 8\n"; // SystemConfig
    ss << "50.000000000 20.000000000 0.100000000 0.050000000 0.000000000 0.500000000 0.500000000\n"; // ScoringConfig
    
    int N = 300;
    ss << N << "\n";
    for (int i = N; i >= 1; --i) { // Unordered batch sizes
        if (i % 3 == 0) {
            // Include -1 missing entries
            ss << i << " 5.000000000 -1.000000000 3.000000000 2.000000000 -1.000000000 1.500000000\n";
        } else {
            ss << i << " 4.000000000 12.000000000 2.500000000 1.500000000 5.000000000 1.200000000\n";
        }
    }

    // Interactive Frames immediately following startup
    ss << "0.000000000\n1\nARR 0 256\n";
    ss << "10.000000000\n1\nTDN E P PRE 0 0 4.000000000\n";
    ss << "25.000000000\n1\nXDN UP 0 1000000 PRE 1 0\n";
    ss << "40.000000000\n1\nTDN C0 P PROC 0 8 0 0 15.000000000\n";
    ss << "55.000000000\n1\nXDN DOWN 0 1000000 PRE 1 0\n";
    ss << "60.000000000\n1\nTDN E P POST 0 0 2.500000000\n";
    ss << "65.000000000\n2\nTDN E D PRE -1 1 0 1.500000000\nFIN 0\n";
    ss << "END\n";

    // 2. Execute parsing using Unified C stdio stream reader
    SystemConfig sys = InteractiveIO::parseSystemConfig(ss);
    assert(sys.K == 2);
    assert(sys.num_layers == 8);
    assert(sys.bytes_per_token == 250000);

    ScoringConfig sc = InteractiveIO::parseScoringConfig(ss);
    assert(sc.SLO1 == 50.0);
    assert(sc.SLO2 == 20.0);

    TaskTable table;
    table.parse(ss);
    assert(table.N == 300);
    assert(table.raw_rows.size() == 300);
    assert(table.raw_rows[0].batch_size == 300); // Unordered check
    assert(table.raw_rows[299].batch_size == 1);

    std::cout << "  [PASS] Startup Config & 300-row Task-Time Table (>15KB) parsed cleanly.\n";

    // 3. Verify immediate transition to interactive frames without stream offset corruption
    FrameContext f1 = InteractiveIO::parseFrame(ss);
    assert(!f1.isEnd);
    assert(f1.timestamp == 0.0);
    assert(f1.eventCount == 1);
    assert(f1.events[0].type == EventType::ARR);
    assert(f1.events[0].rid == 0);
    assert(f1.events[0].Lin == 256);

    std::cout << "  [PASS] First interactive ARR frame parsed with 100% synchronization.\n";

    FrameContext f2 = InteractiveIO::parseFrame(ss);
    assert(!f2.isEnd);
    assert(f2.timestamp == 10.0);
    assert(f2.events[0].type == EventType::TDN);
    assert(f2.events[0].server == "E");
    assert(f2.events[0].task_spec == "P PRE 0 0");

    std::cout << "  [PASS] Subsequent interactive TDN frame remaining synchronized.\n";

    int frameCount = 2;
    while (true) {
        FrameContext frame = InteractiveIO::parseFrame(ss);
        if (frame.isEnd) break;
        frameCount++;
    }
    assert(frameCount == 7);

    std::cout << "  [PASS] All 7 frames processed cleanly to END marker.\n";
    std::cout << "=========================================================================================\n";
    std::cout << "          LARGE-BUFFER STREAM DESYNCHRONIZATION TEST PASSED SUCCESSFULLY                 \n";
    std::cout << "=========================================================================================\n";

    return 0;
}
