#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

#include <string>
#include <vector>
#include <iostream>
#include <sstream>

struct SystemConfig {
    int K = 1;
    double S = 1.0;
    double latency_in_ms = 2.0;
    double bandwidth_gbps = 1.0;
    long long bytes_per_token = 125000;
    int num_layers = 4;
};

struct ScoringConfig {
    double SLO1 = 30.0;
    double SLO2 = 15.0;
    double tp_UB = 0.0625;
    double tp_base = 0.022222222;
    double dist_base = 0.0;
    double w_tp = 0.5;
    double w_c = 0.5;
};

enum class EventType {
    ARR,
    TDN,
    XDN,
    FIN
};

struct Event {
    EventType type;
    int rid = -1;
    int Lin = 0;
    
    // TDN fields
    std::string server;      // "E", "C0", ...
    std::string task_spec;   // Echoed task specification string
    double dur = 0.0;

    // XDN fields
    std::string direction;   // "UP" or "DOWN"
    int remote = 0;
    long long size = 0;
    std::string stage_tag;   // "PRE" or "DEC"
    int m = 0;
    std::vector<int> rids;
};

struct FrameContext {
    double timestamp = 0.0;
    int eventCount = 0;
    std::vector<Event> events;
    bool isEnd = false;
};

class InteractiveIO {
public:
    static SystemConfig parseSystemConfig(std::istream& in);
    static ScoringConfig parseScoringConfig(std::istream& in);
    static FrameContext parseFrame(std::istream& in);
};

#endif // PROTOCOL_HPP
