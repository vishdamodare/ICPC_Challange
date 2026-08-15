#include "reference_strategy.hpp"
#include <algorithm>

std::vector<Task> ReferenceStrategy::selectTasks(const StateTracker& state, const std::vector<Task>& candidates) {
    std::vector<Task> selected;

    // Edge task selection
    Task bestEdgeTask;
    bool foundEdge = false;
    int bestEdgePriority = -1; // Higher is better: 3=P_POST, 2=D_POST, 1=D_PRE, 0=P_PRE

    for (const auto& task : candidates) {
        if (task.server != -1) continue;

        int priority = -1;
        if (task.type == TaskType::P_POST) priority = 3;
        else if (task.type == TaskType::D_POST) priority = 2;
        else if (task.type == TaskType::D_PRE) priority = 1;
        else if (task.type == TaskType::P_PRE) {
            // Check if this P PRE matches round-robin cloud
            int targetRemote = roundRobinCounter % state.sysConfig.K;
            if (task.remote == targetRemote) {
                priority = 0;
            }
        }

        if (priority > bestEdgePriority) {
            bestEdgePriority = priority;
            bestEdgeTask = task;
            foundEdge = true;
        }
    }

    if (foundEdge) {
        selected.push_back(bestEdgeTask);
        if (bestEdgeTask.type == TaskType::P_PRE) {
            roundRobinCounter++;
        }
    }

    // Cloud task selection (per cloud k)
    for (int k = 0; k < state.sysConfig.K; ++k) {
        Task bestCloudTask;
        bool foundCloud = false;
        int bestCloudPriority = -1; // 1=D_PROC, 0=P_PROC

        for (const auto& task : candidates) {
            if (task.server != k) continue;

            int priority = -1;
            if (task.type == TaskType::D_PROC) priority = 1;
            else if (task.type == TaskType::P_PROC) priority = 0;

            if (priority > bestCloudPriority) {
                bestCloudPriority = priority;
                bestCloudTask = task;
                foundCloud = true;
            }
        }

        if (foundCloud) {
            selected.push_back(bestCloudTask);
        }
    }

    return selected;
}
