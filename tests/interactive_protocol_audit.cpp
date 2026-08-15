#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <sstream>
#include <string>

static void testProtocolCRLF() {
    std::cout << "  [PROTOCOL 1] Testing CRLF (\\r\\n) Line Endings ... ";
    std::string mockInput =
        "4 1.0 2.0 1.0 125000 4\r\n"
        "150 30 1.0 1.0 1.0 0.5 0.5\r\n"
        "1\r\n"
        "1 3.0 10.0 2.0 1.0 4.0 1.0\r\n"
        "0.000000000\r\n"
        "1\r\n"
        "ARR 0 4\r\n"
        "END\r\n";

    std::stringstream iss(mockInput);

    SystemConfig sys = InteractiveIO::parseSystemConfig(iss);
    assert(sys.K == 4);

    ScoringConfig sc = InteractiveIO::parseScoringConfig(iss);

    TaskTable table;
    table.parse(iss);
    assert(table.N == 1);

    FrameContext frame = InteractiveIO::parseFrame(iss);
    assert(frame.eventCount == 1);
    assert(frame.events[0].type == EventType::ARR);
    assert(frame.events[0].rid == 0);

    std::cout << "[PASS]\n";
}

static void testProtocolZeroEvents() {
    std::cout << "  [PROTOCOL 2] Testing Zero-Event Frames (eventCount = 0) ... ";
    std::string mockInput =
        "4 1.0 2.0 1.0 125000 4\n"
        "150 30 1.0 1.0 1.0 0.5 0.5\n"
        "1\n"
        "1 3.0 10.0 2.0 1.0 4.0 1.0\n"
        "0.500000000\n"
        "0\n"
        "END\n";

    std::stringstream iss(mockInput);

    SystemConfig sys = InteractiveIO::parseSystemConfig(iss);
    ScoringConfig sc = InteractiveIO::parseScoringConfig(iss);
    TaskTable table;
    table.parse(iss);

    FrameContext frame = InteractiveIO::parseFrame(iss);
    assert(frame.eventCount == 0);
    assert(frame.events.empty());

    std::cout << "[PASS]\n";
}

static void testProtocolEOF() {
    std::cout << "  [PROTOCOL 3] Testing Clean EOF Handling ... ";
    std::string mockInput =
        "4 1.0 2.0 1.0 125000 4\n"
        "150 30 1.0 1.0 1.0 0.5 0.5\n"
        "1\n"
        "1 3.0 10.0 2.0 1.0 4.0 1.0\n"
        "0.500000000\n"
        "0\n"
        "END\n";

    std::stringstream iss(mockInput);

    SystemConfig sys = InteractiveIO::parseSystemConfig(iss);
    ScoringConfig sc = InteractiveIO::parseScoringConfig(iss);
    TaskTable table;
    table.parse(iss);

    FrameContext frame1 = InteractiveIO::parseFrame(iss);
    assert(!frame1.isEnd);

    FrameContext frame2 = InteractiveIO::parseFrame(iss);
    assert(frame2.isEnd);

    std::cout << "[PASS]\n";
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "     GATE 4 INTERACTIVE PROTOCOL AUDIT SUITE         \n";
    std::cout << "=====================================================\n";

    testProtocolCRLF();
    testProtocolZeroEvents();
    testProtocolEOF();

    std::cout << "=====================================================\n";
    std::cout << "       ALL INTERACTIVE PROTOCOL AUDITS PASSED        \n";
    std::cout << "=====================================================\n";

    return 0;
}
