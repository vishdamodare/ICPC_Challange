#include "legal_tasks.hpp"

std::vector<Task> LegalTaskGenerator::generateCandidates(const StateTracker& state) {
    std::vector<Task> candidates;
    candidates.reserve(16);

    // Edge task candidates (if Edge is free)
    if (!state.edgeServer.busy) {
        // P PRE (1 per remote target k)
        if (!state.pPreReadyList.empty()) {
            int rid = state.pPreReadyList[0];
            for (int k = 0; k < state.sysConfig.K; ++k) {
                Task t;
                t.type = TaskType::P_PRE;
                t.server = -1;
                t.remote = k;
                t.m = 1;
                t.requests = {rid};
                candidates.push_back(t);
            }
        }
        // P POST
        if (!state.pPostReadyList.empty()) {
            int rid = state.pPostReadyList[0];
            Task t;
            t.type = TaskType::P_POST;
            t.server = -1;
            t.remote = state.requests[rid].assignedRemote;
            t.m = 1;
            t.requests = {rid};
            candidates.push_back(t);
        }
        // D PRE
        if (!state.dPreReadyList.empty()) {
            Task t;
            t.type = TaskType::D_PRE;
            t.server = -1;
            t.remote = -1;
            t.m = static_cast<int>(state.dPreReadyList.size());
            t.requests = state.dPreReadyList;
            candidates.push_back(t);
        }
        // D POST
        if (!state.dPostReadyList.empty()) {
            Task t;
            t.type = TaskType::D_POST;
            t.server = -1;
            t.remote = -1;
            t.m = static_cast<int>(state.dPostReadyList.size());
            t.requests = state.dPostReadyList;
            candidates.push_back(t);
        }
    }

    // Cloud task candidates (per free cloud server Ck)
    for (int k = 0; k < state.sysConfig.K; ++k) {
        if (state.cloudServers[k].busy) continue;

        // P PROC
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
            candidates.push_back(t);
        }
        // D PROC
        if (!state.dProcReadyList[k].empty()) {
            Task t;
            t.type = TaskType::D_PROC;
            t.server = k;
            t.remote = k;
            t.m = static_cast<int>(state.dProcReadyList[k].size());
            t.requests = state.dProcReadyList[k];
            candidates.push_back(t);
        }
    }

    return candidates;
}
