#include "output.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

static char intBuf[16];
static char g_outStaticBuf[1048576]; // 1MB static output buffer for large decode batches

static inline void appendIntToBuf(char*& p, int val) {
    if (val == 0) {
        *p++ = '0';
        return;
    }
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    int pos = 0;
    while (val > 0) {
        intBuf[pos++] = '0' + (val % 10);
        val /= 10;
    }
    while (pos > 0) {
        *p++ = intBuf[--pos];
    }
}

void OutputWriter::writeAssignments(std::ostream& os, const std::vector<Task>& assignments) {
    if (assignments.empty()) {
        fputs("0\n", stdout);
        fflush(stdout);
        return;
    }

    char* p = g_outStaticBuf;

    appendIntToBuf(p, static_cast<int>(assignments.size()));
    *p++ = '\n';

    for (const auto& task : assignments) {
        if (task.server == -1) {
            *p++ = 'E';
        } else {
            *p++ = 'C';
            appendIntToBuf(p, task.server);
        }
        *p++ = ' ';

        switch (task.type) {
            case TaskType::P_PRE:
                memcpy(p, "P PRE ", 6); p += 6;
                appendIntToBuf(p, task.remote);
                *p++ = ' ';
                appendIntToBuf(p, task.requests[0]);
                break;
            case TaskType::P_PROC:
                memcpy(p, "P PROC ", 7); p += 7;
                appendIntToBuf(p, task.ls);
                *p++ = ' ';
                appendIntToBuf(p, task.le);
                *p++ = ' ';
                appendIntToBuf(p, task.remote);
                *p++ = ' ';
                appendIntToBuf(p, task.requests[0]);
                break;
            case TaskType::P_POST:
                memcpy(p, "P POST ", 7); p += 7;
                appendIntToBuf(p, task.remote);
                *p++ = ' ';
                appendIntToBuf(p, task.requests[0]);
                break;
            case TaskType::D_PRE:
                memcpy(p, "D PRE -1 ", 9); p += 9;
                appendIntToBuf(p, task.m);
                for (int r : task.requests) {
                    *p++ = ' ';
                    appendIntToBuf(p, r);
                }
                break;
            case TaskType::D_PROC:
                memcpy(p, "D PROC ", 7); p += 7;
                appendIntToBuf(p, task.remote);
                *p++ = ' ';
                appendIntToBuf(p, task.m);
                for (int r : task.requests) {
                    *p++ = ' ';
                    appendIntToBuf(p, r);
                }
                break;
            case TaskType::D_POST:
                memcpy(p, "D POST -1 ", 10); p += 10;
                appendIntToBuf(p, task.m);
                for (int r : task.requests) {
                    *p++ = ' ';
                    appendIntToBuf(p, r);
                }
                break;
        }
        *p++ = '\n';
    }
    *p = '\0';
    fputs(g_outStaticBuf, stdout);
    fflush(stdout);
}
