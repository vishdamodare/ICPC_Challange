#include "adaptive_strategy.hpp"
#include <algorithm>
#include <cmath>

int AdaptiveStrategy::computeOptimalBatchSize(TaskStep step, int availableCount, double SLO_target) const {
    if (availableCount <= 1) return availableCount;

    int bestM = 1;
    double maxUtility = -1e9;

    for (int m = 1; m <= availableCount; ++m) {
        double dur = taskTable.getDuration(step, m);
        double totalTaskCost = 1.0 + dur; // S = 1.0 default
        
        // Throughput utility component
        double tpGain = static_cast<double>(m) / totalTaskCost;

        // Latency penalty component against target SLO
        double latencyPenalty = 0.0;
        if (dur > SLO_target && SLO_target > 0) {
            double excess = (dur - SLO_target) / SLO_target;
            latencyPenalty = excess * excess;
        }

        // Combined utility
        double utility = tpGain - 0.5 * latencyPenalty;

        if (utility > maxUtility) {
            maxUtility = utility;
            bestM = m;
        }
    }

    return bestM;
}

std::vector<Task> AdaptiveStrategy::selectTasks(const StateTracker& state, const std::vector<Task>& candidates) {
    std::vector<Task> selected;
    cloudEstimatedFinishTime.assign(state.sysConfig.K, 0.0);

    // Estimate cloud finish times from in-flight workloads
    for (const auto& req : state.requests) {
        if (req.rid >= 0 && !req.finished && req.assignedRemote >= 0 && req.assignedRemote < state.sysConfig.K) {
            if (req.stage == RequestStage::P_PROC_IN_FLIGHT) {
                double dur = taskTable.getDuration(TaskStep::PREFILL_PROC, req.Lin);
                cloudEstimatedFinishTime[req.assignedRemote] += (state.sysConfig.S + dur);
            } else if (req.stage == RequestStage::D_PROC_IN_FLIGHT) {
                double dur = taskTable.getDuration(TaskStep::DECODE_PROC, 1);
                cloudEstimatedFinishTime[req.assignedRemote] += (state.sysConfig.S + dur);
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
            int optM = computeOptimalBatchSize(TaskStep::DECODE_POST, dpostReady.size(), 15.0);
            Task t;
            t.type = TaskType::D_POST;
            t.server = -1;
            t.remote = -1;
            t.m = optM;
            t.requests.assign(dpostReady.begin(), dpostReady.begin() + optM);
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
            int optM = computeOptimalBatchSize(TaskStep::DECODE_PRE, dpreReady.size(), 30.0);
            Task t;
            t.type = TaskType::D_PRE;
            t.server = -1;
            t.remote = -1;
            t.m = optM;
            t.requests.assign(dpreReady.begin(), dpreReady.begin() + optM);
            selected.push_back(t);
        } else if (!ppreReady.empty()) {
            int bestCloud = 0;
            double minEst = cloudEstimatedFinishTime[0];
            for (int k = 1; k < state.sysConfig.K; ++k) {
                if (cloudEstimatedFinishTime[k] < minEst) {
                    minEst = cloudEstimatedFinishTime[k];
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
            int optM = computeOptimalBatchSize(TaskStep::DECODE_PROC, dprocReady.size(), 15.0);
            Task t;
            t.type = TaskType::D_PROC;
            t.server = k;
            t.remote = k;
            t.m = optM;
            t.requests.assign(dprocReady.begin(), dprocReady.begin() + optM);
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
