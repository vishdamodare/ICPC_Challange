#include "state_tracker.hpp"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>

static inline int fastParseInt(const char*& p) {
    while (*p && *p <= ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return neg ? -val : val;
}

static inline void removeFromVec(std::vector<int>& vec, int val) {
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i] == val) {
            vec[i] = vec.back();
            vec.pop_back();
            return;
        }
    }
}

void StateTracker::init(const SystemConfig& sys) {
    sysConfig = sys;
    edgeServer.busy = false;
    cloudServers.resize(sys.K);
    for (int k = 0; k < sys.K; ++k) {
        cloudServers[k].busy = false;
    }
    requests.clear();
    pPreReadyList.clear();
    pPostReadyList.clear();
    dPreReadyList.clear();
    dPostReadyList.clear();
    pProcReadyList.assign(sys.K, {});
    dProcReadyList.assign(sys.K, {});
}

void StateTracker::rebuildReadinessLists() {
    // No-op: readiness lists are maintained incrementally in O(1) on event commits
}

void StateTracker::processFrame(const FrameContext& frame) {
    FrameDelta delta;
    for (const auto& ev : frame.events) {
        switch (ev.type) {
            case EventType::ARR:
                delta.arrivals.push_back(ev);
                break;
            case EventType::TDN:
                delta.taskCompletions.push_back(ev);
                break;
            case EventType::XDN:
                delta.transferCompletions.push_back(ev);
                break;
            case EventType::FIN:
                delta.finishes.push_back(ev);
                break;
        }
    }

    applyArrivals(delta);
    applyTaskCompletions(delta);
    applyTransferCompletions(delta);
    applyFinishes(delta);
}

void StateTracker::applyArrivals(const FrameDelta& delta) {
    for (const auto& ev : delta.arrivals) {
        if (ev.rid >= static_cast<int>(requests.size())) {
            requests.resize(ev.rid + 1);
        }
        RequestState req;
        req.rid = ev.rid;
        req.Lin = ev.Lin;
        req.stage = RequestStage::ARRIVED;
        requests[ev.rid] = req;

        pPreReadyList.push_back(ev.rid);
    }
}

void StateTracker::applyTaskCompletions(const FrameDelta& delta) {
    for (const auto& ev : delta.taskCompletions) {
        int cloudIdx = -1;
        if (ev.server[0] == 'E' && ev.server[1] == '\0') {
            edgeServer.busy = false;
        } else if (ev.server[0] == 'C') {
            int k = 0;
            const char* sp = ev.server + 1;
            while (*sp >= '0' && *sp <= '9') {
                k = k * 10 + (*sp - '0');
                sp++;
            }
            if (k >= 0 && k < sysConfig.K) {
                cloudServers[k].busy = false;
                cloudIdx = k;
            }
        }

        const char* ptr = ev.task_spec;
        while (*ptr && *ptr <= ' ') ptr++;

        if (ptr[0] == 'P') {
            ptr++;
            while (*ptr && *ptr <= ' ') ptr++;
            if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'E') {
                ptr += 3;
                int remote = fastParseInt(ptr);
                int rid = fastParseInt(ptr);
                if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                    requests[rid].assignedRemote = remote;
                    requests[rid].stage = RequestStage::P_WAIT_UP;
                }
            } else if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'O' && ptr[3] == 'C') {
                ptr += 4;
                int ls = fastParseInt(ptr);
                int le = fastParseInt(ptr);
                int remote = fastParseInt(ptr);
                int rid = fastParseInt(ptr);
                if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                    if (le < sysConfig.num_layers) {
                        requests[rid].nextLayerStart = le;
                        requests[rid].stage = RequestStage::P_PROC_READY;
                        if (cloudIdx >= 0 && cloudIdx < sysConfig.K) {
                            pProcReadyList[cloudIdx].push_back(rid);
                        }
                    } else {
                        requests[rid].nextLayerStart = sysConfig.num_layers;
                        requests[rid].stage = RequestStage::P_WAIT_DOWN;
                    }
                }
            } else if (ptr[0] == 'P' && ptr[1] == 'O' && ptr[2] == 'S' && ptr[3] == 'T') {
                ptr += 4;
                int remote = fastParseInt(ptr);
                int rid = fastParseInt(ptr);
                if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                    requests[rid].stage = RequestStage::D_PRE_READY;
                    dPreReadyList.push_back(rid);
                }
            }
        } else if (ptr[0] == 'D') {
            ptr++;
            while (*ptr && *ptr <= ' ') ptr++;
            if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'E') {
                ptr += 3;
                int dummy_remote = fastParseInt(ptr);
                int m = fastParseInt(ptr);
                for (int i = 0; i < m; ++i) {
                    int rid = fastParseInt(ptr);
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].stage = RequestStage::D_WAIT_UP;
                        requests[rid].decodeUpReady = false;
                    }
                }
            } else if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'O' && ptr[3] == 'C') {
                ptr += 4;
                int remote = fastParseInt(ptr);
                int m = fastParseInt(ptr);
                for (int i = 0; i < m; ++i) {
                    int rid = fastParseInt(ptr);
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].stage = RequestStage::D_WAIT_DOWN;
                        requests[rid].decodeDownReady = false;
                    }
                }
            } else if (ptr[0] == 'P' && ptr[1] == 'O' && ptr[2] == 'S' && ptr[3] == 'T') {
                ptr += 4;
                int dummy_remote = fastParseInt(ptr);
                int m = fastParseInt(ptr);
                for (int i = 0; i < m; ++i) {
                    int rid = fastParseInt(ptr);
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].tokensProduced++;
                        requests[rid].decodeIteration++;
                        if (!requests[rid].finished) {
                            requests[rid].stage = RequestStage::D_PRE_READY;
                            dPreReadyList.push_back(rid);
                        }
                    }
                }
            }
        }
    }
}

void StateTracker::applyTransferCompletions(const FrameDelta& delta) {
    for (const auto& ev : delta.transferCompletions) {
        if (strcmp(ev.stage_tag, "PRE") == 0) {
            if (strcmp(ev.direction, "UP") == 0) {
                if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
                    requests[ev.rid].stage = RequestStage::P_PROC_READY;
                    int k = requests[ev.rid].assignedRemote;
                    if (k >= 0 && k < sysConfig.K) {
                        pProcReadyList[k].push_back(ev.rid);
                    }
                }
            } else if (strcmp(ev.direction, "DOWN") == 0) {
                if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
                    requests[ev.rid].stage = RequestStage::P_POST_READY;
                    pPostReadyList.push_back(ev.rid);
                }
            }
        } else if (strcmp(ev.stage_tag, "DEC") == 0) {
            if (strcmp(ev.direction, "UP") == 0) {
                for (int i = 0; i < ev.m; ++i) {
                    int rid = ev.rids[i];
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].decodeUpReady = true;
                        requests[rid].stage = RequestStage::D_PROC_READY;
                        int k = requests[rid].assignedRemote;
                        if (k >= 0 && k < sysConfig.K) {
                            dProcReadyList[k].push_back(rid);
                        }
                    }
                }
            } else if (strcmp(ev.direction, "DOWN") == 0) {
                for (int i = 0; i < ev.m; ++i) {
                    int rid = ev.rids[i];
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].decodeDownReady = true;
                        requests[rid].stage = RequestStage::D_POST_READY;
                        dPostReadyList.push_back(rid);
                    }
                }
            }
        }
    }
}

void StateTracker::applyFinishes(const FrameDelta& delta) {
    for (const auto& ev : delta.finishes) {
        if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
            requests[ev.rid].finished = true;
            requests[ev.rid].stage = RequestStage::FINISHED;
            removeFromVec(dPreReadyList, ev.rid);
            removeFromVec(dPostReadyList, ev.rid);
        }
    }
}

void StateTracker::markTaskAssigned(const Task& task) {
    if (task.server == -1) {
        edgeServer.busy = true;
    } else if (task.server >= 0 && task.server < sysConfig.K) {
        cloudServers[task.server].busy = true;
    }

    for (int rid : task.requests) {
        if (rid >= 0 && rid < static_cast<int>(requests.size())) {
            switch (task.type) {
                case TaskType::P_PRE:
                    requests[rid].stage = RequestStage::P_PRE_IN_FLIGHT;
                    removeFromVec(pPreReadyList, rid);
                    break;
                case TaskType::P_PROC:
                    requests[rid].stage = RequestStage::P_PROC_IN_FLIGHT;
                    if (task.server >= 0 && task.server < sysConfig.K) {
                        removeFromVec(pProcReadyList[task.server], rid);
                    }
                    break;
                case TaskType::P_POST:
                    removeFromVec(pPostReadyList, rid);
                    break;
                case TaskType::D_PRE:
                    requests[rid].stage = RequestStage::D_PRE_IN_FLIGHT;
                    removeFromVec(dPreReadyList, rid);
                    break;
                case TaskType::D_PROC:
                    requests[rid].stage = RequestStage::D_PROC_IN_FLIGHT;
                    if (task.server >= 0 && task.server < sysConfig.K) {
                        removeFromVec(dProcReadyList[task.server], rid);
                    }
                    break;
                case TaskType::D_POST:
                    requests[rid].stage = RequestStage::D_POST_IN_FLIGHT;
                    removeFromVec(dPostReadyList, rid);
                    break;
            }
        }
    }
}
