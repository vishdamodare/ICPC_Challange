#ifndef STATE_TRACKER_HPP
#define STATE_TRACKER_HPP

#include "protocol.hpp"
#include "task.hpp"
#include <vector>
#include <string>

enum class RequestStage {
    ARRIVED,
    P_PRE_IN_FLIGHT,
    P_WAIT_UP,
    P_PROC_READY,
    P_PROC_IN_FLIGHT,
    P_WAIT_DOWN,
    P_POST_READY,
    D_PRE_READY,
    D_PRE_IN_FLIGHT,
    D_WAIT_UP,
    D_PROC_READY,
    D_PROC_IN_FLIGHT,
    D_WAIT_DOWN,
    D_POST_READY,
    D_POST_IN_FLIGHT,
    FINISHED
};

struct RequestState {
    int rid = -1;
    int Lin = 0;
    int assignedRemote = -1;

    int nextLayerStart = 0;
    int tokensProduced = 0;
    uint64_t decodeIteration = 0;

    RequestStage stage = RequestStage::ARRIVED;

    bool decodeUpReady = false;
    bool decodeDownReady = false;
    bool finished = false;
};

struct ServerState {
    bool busy = false;
    std::string currentTaskSpec;
};

struct FrameDelta {
    std::vector<Event> arrivals;
    std::vector<Event> taskCompletions;
    std::vector<Event> transferCompletions;
    std::vector<Event> finishes;
};

class StateTracker {
public:
    SystemConfig sysConfig;
    std::vector<RequestState> requests; // O(1) indexed by rid
    ServerState edgeServer;
    std::vector<ServerState> cloudServers;

    // Fast indexed readiness containers (updated on event commits)
    std::vector<int> pPreReadyList;
    std::vector<int> pPostReadyList;
    std::vector<int> dPreReadyList;
    std::vector<int> dPostReadyList;
    std::vector<std::vector<int>> pProcReadyList; // size K
    std::vector<std::vector<int>> dProcReadyList; // size K

    void init(const SystemConfig& sys);
    void processFrame(const FrameContext& frame);
    void markTaskAssigned(const Task& task);
    void rebuildReadinessLists();

private:
    void applyArrivals(const FrameDelta& delta);
    void applyTaskCompletions(const FrameDelta& delta);
    void applyTransferCompletions(const FrameDelta& delta);
    void applyFinishes(const FrameDelta& delta);
};

#endif // STATE_TRACKER_HPP
