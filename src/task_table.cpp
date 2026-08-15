#include "task_table.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

static bool getNextLine(std::istream& is, char* buf, size_t size) {
    if (&is == &std::cin) {
        return fgets(buf, size, stdin) != nullptr;
    } else {
        if (is.getline(buf, size)) {
            return true;
        }
        return false;
    }
}

void TaskTable::parse(std::istream& in) {
    static char lineBuf[1024];
    if (getNextLine(in, lineBuf, sizeof(lineBuf))) {
        N = std::strtol(lineBuf, nullptr, 10);
        raw_rows.resize(N);
        for (int i = 0; i < N; ++i) {
            if (getNextLine(in, lineBuf, sizeof(lineBuf))) {
                sscanf(lineBuf, "%d %lf %lf %lf %lf %lf %lf",
                       &raw_rows[i].batch_size,
                       &raw_rows[i].prefill_pre,
                       &raw_rows[i].prefill_proc,
                       &raw_rows[i].prefill_post,
                       &raw_rows[i].decode_pre,
                       &raw_rows[i].decode_proc,
                       &raw_rows[i].decode_post);
            }
        }
    }
}

std::vector<TaskTable::StepPoint> TaskTable::getSortedPoints(TaskStep step) const {
    std::vector<StepPoint> points;
    for (const auto& row : raw_rows) {
        double val = -1.0;
        switch (step) {
            case TaskStep::PREFILL_PRE: val = row.prefill_pre; break;
            case TaskStep::PREFILL_PROC: val = row.prefill_proc; break;
            case TaskStep::PREFILL_POST: val = row.prefill_post; break;
            case TaskStep::DECODE_PRE: val = row.decode_pre; break;
            case TaskStep::DECODE_PROC: val = row.decode_proc; break;
            case TaskStep::DECODE_POST: val = row.decode_post; break;
        }
        if (val >= 0.0) {
            points.push_back({row.batch_size, val});
        }
    }
    std::sort(points.begin(), points.end(), [](const StepPoint& a, const StepPoint& b) {
        return a.size < b.size;
    });
    return points;
}

double TaskTable::interpolate(const std::vector<StepPoint>& points, int batch_size) const {
    if (points.empty()) return 0.0;
    if (batch_size <= points.front().size) return points.front().dur;
    if (batch_size >= points.back().size) return points.back().dur;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (batch_size == points[i].size) return points[i].dur;
        if (batch_size > points[i].size && batch_size < points[i+1].size) {
            double frac = static_cast<double>(batch_size - points[i].size) / (points[i+1].size - points[i].size);
            return points[i].dur + frac * (points[i+1].dur - points[i].dur);
        }
    }
    return points.back().dur;
}

double TaskTable::getDuration(TaskStep step, int batch_size) const {
    auto points = getSortedPoints(step);
    return interpolate(points, batch_size);
}
