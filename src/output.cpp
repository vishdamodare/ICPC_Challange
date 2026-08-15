#include "output.hpp"
#include <unistd.h>
#include <string>

static char intBuf[16];

static inline void appendInt(std::string& s, int val) {
    if (val == 0) {
        s += '0';
        return;
    }
    if (val < 0) {
        s += '-';
        val = -val;
    }
    int pos = 0;
    while (val > 0) {
        intBuf[pos++] = '0' + (val % 10);
        val /= 10;
    }
    while (pos > 0) {
        s += intBuf[--pos];
    }
}

void OutputWriter::writeAssignments(std::ostream& os, const std::vector<Task>& assignments) {
    if (assignments.empty()) {
        static const char zeroResp[] = "0\n";
        write(STDOUT_FILENO, zeroResp, 2);
        return;
    }

    std::string outBuf;
    outBuf.reserve(256);

    appendInt(outBuf, static_cast<int>(assignments.size()));
    outBuf += '\n';

    for (const auto& task : assignments) {
        if (task.server == -1) {
            outBuf += 'E';
        } else {
            outBuf += 'C';
            appendInt(outBuf, task.server);
        }
        outBuf += ' ';

        switch (task.type) {
            case TaskType::P_PRE:
                outBuf += "P PRE ";
                appendInt(outBuf, task.remote);
                outBuf += ' ';
                appendInt(outBuf, task.requests[0]);
                break;
            case TaskType::P_PROC:
                outBuf += "P PROC ";
                appendInt(outBuf, task.ls);
                outBuf += ' ';
                appendInt(outBuf, task.le);
                outBuf += ' ';
                appendInt(outBuf, task.remote);
                outBuf += ' ';
                appendInt(outBuf, task.requests[0]);
                break;
            case TaskType::P_POST:
                outBuf += "P POST ";
                appendInt(outBuf, task.remote);
                outBuf += ' ';
                appendInt(outBuf, task.requests[0]);
                break;
            case TaskType::D_PRE:
                outBuf += "D PRE -1 ";
                appendInt(outBuf, task.m);
                for (int r : task.requests) {
                    outBuf += ' ';
                    appendInt(outBuf, r);
                }
                break;
            case TaskType::D_PROC:
                outBuf += "D PROC ";
                appendInt(outBuf, task.remote);
                outBuf += ' ';
                appendInt(outBuf, task.m);
                for (int r : task.requests) {
                    outBuf += ' ';
                    appendInt(outBuf, r);
                }
                break;
            case TaskType::D_POST:
                outBuf += "D POST -1 ";
                appendInt(outBuf, task.m);
                for (int r : task.requests) {
                    outBuf += ' ';
                    appendInt(outBuf, r);
                }
                break;
        }
        outBuf += '\n';
    }

    write(STDOUT_FILENO, outBuf.data(), outBuf.size());
}
