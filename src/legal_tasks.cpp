#include "legal_tasks.hpp"

std::vector<Task> LegalTaskGenerator::generateCandidates(const StateTracker& state) {
    std::vector<Task> candidates;

    // Edge task candidates (if Edge is free)
    if (!state.edgeServer.busy) {
        // P PRE
        for (int rid : state.pPreReadyList) {
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
        for (int rid : state.pPostReadyList) {
            Task t;
            t.type = TaskType::P_POST;
            t.server = -1;
            t.remote = state.requests[rid].assignedRemote;
            t.m = 1;
            t.requests = {rid};
            candidates.push_back(t);
        }
        // D PRE
        for (int rid : state.dPreReadyList) {
            Task t;
            t.type = TaskType::D_PRE;
            t.server = -1;
            t.remote = -1;
            t.m = 1;
            t.requests = {rid};
            candidates.push_back(t);
        }
        // D POST
        for (int rid : state.dPostReadyList) {
            Task t;
            t.type = TaskType::D_POST;
            t.server = -1;
            t.remote = -1;
            t.m = 1;
            t.requests = {rid};
            candidates.push_back(t);
        }
    }

    // Cloud task candidates (per free cloud server Ck)
    for (int k = 0; k < state.sysConfig.K; ++k) {
        if (state.cloudServers[k].busy) continue;

        // P PROC
        for (int rid : state.pProcReadyList[k]) {
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
        for (int rid : state.dProcReadyList[k]) {
            Task t;
            t.type = TaskType::D_PROC;
            t.server = k;
            t.remote = k;
            t.m = 1;
            t.requests = {rid};
            candidates.push_back(t);
        }
    }

    return candidates;
}
