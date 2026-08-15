#ifndef TASK_HPP
#define TASK_HPP

#include <vector>
#include <string>

enum class TaskType {
    P_PRE,
    P_PROC,
    P_POST,
    D_PRE,
    D_PROC,
    D_POST
};

struct Task {
    TaskType type;
    int server = -1; // -1 for Edge 'E', k >= 0 for Cloud 'Ck'
    int remote = 0;  // Assigned remote computer index [0, K)
    int ls = 0;      // Prefill layer start
    int le = 0;      // Prefill layer end
    int m = 0;       // Group member count
    std::vector<int> requests; // Distinct request IDs
};

#endif // TASK_HPP
