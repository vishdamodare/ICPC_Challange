#ifndef TASK_TABLE_HPP
#define TASK_TABLE_HPP

#include <vector>
#include <iostream>
#include <string>

enum class TaskStep {
    PREFILL_PRE,
    PREFILL_PROC,
    PREFILL_POST,
    DECODE_PRE,
    DECODE_PROC,
    DECODE_POST
};

struct TaskTableRow {
    int batch_size;
    double prefill_pre;
    double prefill_proc;
    double prefill_post;
    double decode_pre;
    double decode_proc;
    double decode_post;
};

class TaskTable {
public:
    int N = 0;
    std::vector<TaskTableRow> raw_rows;

    void parse(std::istream& in);
    double getDuration(TaskStep step, int batch_size) const;

private:
    struct StepPoint {
        int size;
        double dur;
    };
    
    std::vector<StepPoint> getSortedPoints(TaskStep step) const;
    double interpolate(const std::vector<StepPoint>& points, int batch_size) const;
};

#endif // TASK_TABLE_HPP
