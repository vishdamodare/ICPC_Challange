#include "conflict_resolver.hpp"
#include <unordered_set>

std::vector<Task> ConflictResolver::resolveConflicts(const StateTracker& state, const std::vector<Task>& proposedTasks) {
    std::vector<Task> validTasks;
    bool edgeBusyInResponse = state.edgeServer.busy;
    std::vector<bool> cloudBusyInResponse(state.sysConfig.K, false);
    for (int k = 0; k < state.sysConfig.K; ++k) {
        cloudBusyInResponse[k] = state.cloudServers[k].busy;
    }

    std::unordered_set<int> usedRequests;

    for (const auto& task : proposedTasks) {
        // Check server availability
        if (task.server == -1) {
            if (edgeBusyInResponse) continue;
        } else if (task.server >= 0 && task.server < state.sysConfig.K) {
            if (cloudBusyInResponse[task.server]) continue;
        } else {
            continue; // Invalid server index
        }

        // Check request availability & validity
        bool conflict = false;
        for (int rid : task.requests) {
            if (usedRequests.count(rid)) {
                conflict = true;
                break;
            }
            if (rid < 0 || rid >= static_cast<int>(state.requests.size()) || state.requests[rid].finished) {
                conflict = true;
                break;
            }
        }
        if (conflict) continue;

        // Reserve resources & requests
        if (task.server == -1) {
            edgeBusyInResponse = true;
        } else {
            cloudBusyInResponse[task.server] = true;
        }
        for (int rid : task.requests) {
            usedRequests.insert(rid);
        }

        validTasks.push_back(task);
    }

    return validTasks;
}
