#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

#include <string>
#include <vector>
#include <iostream>

struct SystemConfig {
    int K = 0;
    double S = 0.0;
    double latency_in_ms = 0.0;
    double bandwidth_gbps = 0.0;
    long long bytes_per_token = 0;
    int num_layers = 0;
};

struct ScoringConfig {
    double SLO1 = 0.0;
    double SLO2 = 0.0;
    double tp_UB = 0.0;
    double tp_base = 0.0;
    double dist_base = 0.0;
    double w_tp = 0.0;
    double w_c = 0.0;
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
    char server[32] = {0};
    std::string task_spec;
    double dur = 0.0;
    char direction[8] = {0};
    int remote = -1;
    long long size = 0;
    char stage_tag[8] = {0};
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
    static SystemConfig parseSystemConfig(std::istream& is);
    static ScoringConfig parseScoringConfig(std::istream& is);
    static FrameContext parseFrame(std::istream& is);
};

#endif // PROTOCOL_HPP
