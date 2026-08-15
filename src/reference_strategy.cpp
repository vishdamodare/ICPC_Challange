#include "reference_strategy.hpp"
#include <algorithm>

std::vector<Task> ReferenceStrategy::selectTasks(const StateTracker& state, const std::vector<Task>& candidates) {
    std::vector<Task> selected;

    // 1. Edge task selection (Edge max 1 task per turn)
    if (!state.edgeServer.busy) {
        const auto& dpostReady = state.dPostReadyList;
        const auto& ppostReady = state.pPostReadyList;
        const auto& dpreReady = state.dPreReadyList;
        const auto& ppreReady = state.pPreReadyList;

        if (!dpostReady.empty()) {
            Task t;
            t.type = TaskType::D_POST;
            t.server = -1;
            t.remote = -1;
            t.m = 1;
            t.requests = {dpostReady[0]};
            selected.push_back(t);
        } else if (!ppostReady.empty()) {
            Task t;
            t.type = TaskType::P_POST;
            t.server = -1;
            t.remote = state.requests[ppostReady[0]].assignedRemote;
            t.m = 1;
            t.requests = {ppostReady[0]};
            selected.push_back(t);
        } else if (!dpreReady.empty()) {
            Task t;
            t.type = TaskType::D_PRE;
            t.server = -1;
            t.remote = -1;
            t.m = 1;
            t.requests = {dpreReady[0]};
            selected.push_back(t);
        } else if (!ppreReady.empty()) {
            int targetRemote = roundRobinCounter % state.sysConfig.K;
            roundRobinCounter++;
            Task t;
            t.type = TaskType::P_PRE;
            t.server = -1;
            t.remote = targetRemote;
            t.m = 1;
            t.requests = {ppreReady[0]};
            selected.push_back(t);
        }
    }

    // 2. Cloud task selection (per cloud k)
    for (int k = 0; k < state.sysConfig.K; ++k) {
        if (!state.cloudServers[k].busy) {
            const auto& dprocReady = state.dProcReadyList[k];
            const auto& pprocReady = state.pProcReadyList[k];

            if (!dprocReady.empty()) {
                Task t;
                t.type = TaskType::D_PROC;
                t.server = k;
                t.remote = k;
                t.m = 1;
                t.requests = {dprocReady[0]};
                selected.push_back(t);
            } else if (!pprocReady.empty()) {
                Task t;
                t.type = TaskType::P_PROC;
                t.server = k;
                t.remote = k;
                t.ls = state.requests[pprocReady[0]].nextLayerStart;
                t.le = state.sysConfig.num_layers;
                t.m = 1;
                t.requests = {pprocReady[0]};
                selected.push_back(t);
            }
        }
    }

    return selected;
}
