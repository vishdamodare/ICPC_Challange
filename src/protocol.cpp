#include "protocol.hpp"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>

static char lineBuf[1024];

static inline bool getNextLine(std::istream& is, char* buf, size_t size) {
    if (&is == &std::cin) {
        return fgets(buf, size, stdin) != nullptr;
    } else {
        if (is.getline(buf, size)) {
            return true;
        }
        return false;
    }
}

SystemConfig InteractiveIO::parseSystemConfig(std::istream& is) {
    SystemConfig sys;
    if (getNextLine(is, lineBuf, sizeof(lineBuf))) {
        sscanf(lineBuf, "%d %lf %lf %lf %lld %d",
               &sys.K, &sys.S, &sys.latency_in_ms, &sys.bandwidth_gbps, &sys.bytes_per_token, &sys.num_layers);
    }
    return sys;
}

ScoringConfig InteractiveIO::parseScoringConfig(std::istream& is) {
    ScoringConfig sc;
    if (getNextLine(is, lineBuf, sizeof(lineBuf))) {
        sscanf(lineBuf, "%lf %lf %lf %lf %lf %lf %lf",
               &sc.SLO1, &sc.SLO2, &sc.tp_UB, &sc.tp_base, &sc.dist_base, &sc.w_tp, &sc.w_c);
    }
    return sc;
}

static inline int fastParseInt(const char*& p) {
    while (*p && *p <= ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return neg ? -val : val;
}

static inline long long fastParseLongLong(const char*& p) {
    while (*p && *p <= ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    long long val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return neg ? -val : val;
}

static inline double fastParseDouble(const char*& p) {
    while (*p && *p <= ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    double integerPart = 0.0;
    while (*p >= '0' && *p <= '9') {
        integerPart = integerPart * 10.0 + (*p - '0');
        p++;
    }
    double fracPart = 0.0;
    double scale = 0.1;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            fracPart += (*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }
    double res = integerPart + fracPart;
    return neg ? -res : res;
}

FrameContext InteractiveIO::parseFrame(std::istream& is) {
    FrameContext frame;
    
    if (!getNextLine(is, lineBuf, sizeof(lineBuf))) {
        frame.isEnd = true;
        return frame;
    }

    if (lineBuf[0] == 'E' && lineBuf[1] == 'N' && lineBuf[2] == 'D') {
        frame.isEnd = true;
        return frame;
    }

    const char* ptr = lineBuf;
    frame.timestamp = fastParseDouble(ptr);

    if (!getNextLine(is, lineBuf, sizeof(lineBuf))) {
        frame.isEnd = true;
        return frame;
    }

    ptr = lineBuf;
    int count = fastParseInt(ptr);
    frame.eventCount = count;
    frame.events.reserve(count);

    for (int i = 0; i < count; ++i) {
        if (!getNextLine(is, lineBuf, sizeof(lineBuf))) break;

        Event ev;
        ptr = lineBuf;
        while (*ptr && *ptr <= ' ') ptr++;

        if (ptr[0] == 'A' && ptr[1] == 'R' && ptr[2] == 'R') {
            ptr += 3;
            ev.type = EventType::ARR;
            ev.rid = fastParseInt(ptr);
            ev.Lin = fastParseInt(ptr);
        } else if (ptr[0] == 'T' && ptr[1] == 'D' && ptr[2] == 'N') {
            ptr += 3;
            ev.type = EventType::TDN;
            while (*ptr && *ptr <= ' ') ptr++;
            
            const char* startServer = ptr;
            while (*ptr && *ptr > ' ') ptr++;
            ev.server.assign(startServer, ptr - startServer);

            const char* rest = ptr;
            while (*rest && *rest <= ' ') rest++;
            const char* lastSpace = strrchr(rest, ' ');
            if (lastSpace) {
                ev.task_spec.assign(rest, lastSpace - rest);
                const char* durPtr = lastSpace + 1;
                ev.dur = fastParseDouble(durPtr);
            } else {
                ev.task_spec = rest;
                ev.dur = 0.0;
            }
        } else if (ptr[0] == 'X' && ptr[1] == 'D' && ptr[2] == 'N') {
            ptr += 3;
            ev.type = EventType::XDN;
            while (*ptr && *ptr <= ' ') ptr++;
            
            if (ptr[0] == 'U' && ptr[1] == 'P') {
                ev.direction = "UP";
                ptr += 2;
            } else if (ptr[0] == 'D' && ptr[1] == 'O' && ptr[2] == 'W' && ptr[3] == 'N') {
                ev.direction = "DOWN";
                ptr += 4;
            }

            ev.remote = fastParseInt(ptr);
            ev.size = fastParseLongLong(ptr);
            
            while (*ptr && *ptr <= ' ') ptr++;
            const char* startTag = ptr;
            while (*ptr && *ptr > ' ') ptr++;
            ev.stage_tag.assign(startTag, ptr - startTag);

            ev.m = fastParseInt(ptr);
            ev.rids.resize(ev.m);
            for (int r = 0; r < ev.m; ++r) {
                ev.rids[r] = fastParseInt(ptr);
            }
            if (!ev.rids.empty()) ev.rid = ev.rids[0];
        } else if (ptr[0] == 'F' && ptr[1] == 'I' && ptr[2] == 'N') {
            ptr += 3;
            ev.type = EventType::FIN;
            ev.rid = fastParseInt(ptr);
        }

        frame.events.push_back(ev);
    }

    return frame;
}
