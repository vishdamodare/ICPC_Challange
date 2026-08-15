#include "greedy_strategy.h"
#include <algorithm>

namespace scheduling {

std::vector<Task> GreedyBatchStrategy::selectTasks(const StateTracker& state, const std::vector<Task>& candidates) {
    (void)candidates;
    std::vector<Task> selected;
    
    // Edge server priority: P_POST -> P_PRE -> D_POST -> D_PRE
    if (!state.edgeServer.busy) {
        if (!state.pPostReadyList.empty()) {
            int rid = state.pPostReadyList[0];
            Task t;
            t.type = TaskType::P_POST;
            t.server = -1;
            t.remote = state.requests[rid].assignedRemote;
            t.m = 1;
            t.requests = {rid};
            selected.push_back(t);
        } else if (!state.pPreReadyList.empty()) {
            int rid = state.pPreReadyList[0];
            int targetRemote = rid % state.sysConfig.K;
            Task t;
            t.type = TaskType::P_PRE;
            t.server = -1;
            t.remote = targetRemote;
            t.m = 1;
            t.requests = {rid};
            selected.push_back(t);
        } else if (!state.dPostReadyList.empty()) {
            Task t;
            t.type = TaskType::D_POST;
            t.server = -1;
            t.remote = -1;
            t.m = static_cast<int>(state.dPostReadyList.size());
            t.requests = state.dPostReadyList;
            selected.push_back(t);
        } else if (!state.dPreReadyList.empty()) {
            Task t;
            t.type = TaskType::D_PRE;
            t.server = -1;
            t.remote = -1;
            t.m = static_cast<int>(state.dPreReadyList.size());
            t.requests = state.dPreReadyList;
            selected.push_back(t);
        }
    }
    
    // Cloud servers priority: P_PROC -> D_PROC
    for (int k = 0; k < state.sysConfig.K; ++k) {
        if (state.cloudServers[k].busy) continue;
        
        if (!state.pProcReadyList[k].empty()) {
            int rid = state.pProcReadyList[k][0];
            Task t;
            t.type = TaskType::P_PROC;
            t.server = k;
            t.remote = k;
            t.ls = state.requests[rid].nextLayerStart;
            t.le = state.sysConfig.num_layers;
            t.m = 1;
            t.requests = {rid};
            selected.push_back(t);
        } else if (!state.dProcReadyList[k].empty()) {
            Task t;
            t.type = TaskType::D_PROC;
            t.server = k;
            t.remote = k;
            t.m = static_cast<int>(state.dProcReadyList[k].size());
            t.requests = state.dProcReadyList[k];
            selected.push_back(t);
        }
    }
    
    return selected;
}

} // namespace scheduling
