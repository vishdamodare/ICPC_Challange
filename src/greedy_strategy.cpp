#include "greedy_strategy.hpp"
#include <algorithm>

std::vector<Task> GreedyBatchStrategy::selectTasks(const StateTracker& state, const std::vector<Task>& candidates) {
    std::vector<Task> selected;
    cloudWorkload.assign(state.sysConfig.K, 0.0);

    // Predict cloud estimated finish times based on layer count & token lengths
    for (const auto& req : state.requests) {
        if (req.rid >= 0 && !req.finished && req.assignedRemote >= 0 && req.assignedRemote < state.sysConfig.K) {
            if (req.stage == RequestStage::P_PROC_IN_FLIGHT) {
                double dur = taskTable.getDuration(TaskStep::PREFILL_PROC, req.Lin);
                cloudWorkload[req.assignedRemote] += (state.sysConfig.S + dur);
            } else if (req.stage == RequestStage::D_PROC_IN_FLIGHT) {
                double dur = taskTable.getDuration(TaskStep::DECODE_PROC, 1);
                cloudWorkload[req.assignedRemote] += (state.sysConfig.S + dur);
            }
        }
    }

    // 1. Edge Task Selection (Edge max 1 task per turn)
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
            t.m = dpostReady.size();
            t.requests = dpostReady;
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
            t.m = dpreReady.size();
            t.requests = dpreReady;
            selected.push_back(t);
        } else if (!ppreReady.empty()) {
            int bestCloud = 0;
            double minWork = cloudWorkload[0];
            for (int k = 1; k < state.sysConfig.K; ++k) {
                if (cloudWorkload[k] < minWork) {
                    minWork = cloudWorkload[k];
                    bestCloud = k;
                }
            }
            Task t;
            t.type = TaskType::P_PRE;
            t.server = -1;
            t.remote = bestCloud;
            t.m = 1;
            t.requests = {ppreReady[0]};
            selected.push_back(t);
        }
    }

    // 2. Cloud Task Selection (per free cloud Ck)
    for (int k = 0; k < state.sysConfig.K; ++k) {
        if (state.cloudServers[k].busy) continue;

        const auto& dprocReady = state.dProcReadyList[k];
        const auto& pprocReady = state.pProcReadyList[k];

        if (!dprocReady.empty()) {
            Task t;
            t.type = TaskType::D_PROC;
            t.server = k;
            t.remote = k;
            t.m = dprocReady.size();
            t.requests = dprocReady;
            selected.push_back(t);
        } else if (!pprocReady.empty()) {
            int rid = pprocReady[0];
            Task t;
            t.type = TaskType::P_PROC;
            t.server = k;
            t.remote = k;
            t.ls = state.requests[rid].nextLayerStart;
            t.le = state.sysConfig.num_layers;
            t.m = 1;
            t.requests = {rid};
            selected.push_back(t);
        }
    }

    return selected;
}
