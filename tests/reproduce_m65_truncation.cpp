#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <algorithm>

// ============================================================================
// OLD SOLVER LOGIC (contest-final-v1.0: rids[64] and task_spec[64] fixed arrays)
// ============================================================================

namespace OldSolver {

struct Event {
    int type;
    int rid;
    int Lin;
    char server[8];
    char task_spec[64];
    double dur;
    char direction[8];
    int remote;
    long long size;
    char stage_tag[8];
    int m;
    int rids[64]; // FIXED 64-ELEMENT ARRAY IN CONTEST-FINAL-V1.0
};

struct RequestState {
    int rid = -1;
    bool decodeDownReady = false;
    int stage = 0;
};

class StateTracker {
public:
    std::vector<RequestState> requests;
    std::vector<int> dPostReadyList;

    void applyTransferCompletions(const std::vector<Event>& completions) {
        for (const auto& ev : completions) {
            if (strcmp(ev.stage_tag, "DEC") == 0 && strcmp(ev.direction, "DOWN") == 0) {
                // OLD CODE IN CONTEST-FINAL-V1.0: fetchCount capped at 64!
                int fetchCount = std::min<int>(ev.m, 64);
                for (int i = 0; i < fetchCount; ++i) {
                    int rid = ev.rids[i];
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].decodeDownReady = true;
                        requests[rid].stage = 1; // D_POST_READY
                        dPostReadyList.push_back(rid);
                    }
                }
            }
        }
    }
};

} // namespace OldSolver

// ============================================================================
// NEW SOLVER LOGIC (current version: std::vector<int> rids)
// ============================================================================

namespace NewSolver {

struct Event {
    int type;
    int rid;
    int Lin;
    char server[32];
    std::string task_spec;
    double dur;
    char direction[8];
    int remote;
    long long size;
    char stage_tag[8];
    int m;
    std::vector<int> rids; // DYNAMIC VECTOR
};

struct RequestState {
    int rid = -1;
    bool decodeDownReady = false;
    int stage = 0;
};

class StateTracker {
public:
    std::vector<RequestState> requests;
    std::vector<int> dPostReadyList;

    void applyTransferCompletions(const std::vector<Event>& completions) {
        for (const auto& ev : completions) {
            if (strcmp(ev.stage_tag, "DEC") == 0 && strcmp(ev.direction, "DOWN") == 0) {
                // NEW CODE: Iterate through ALL ev.rids.size()
                for (size_t i = 0; i < ev.rids.size(); ++i) {
                    int rid = ev.rids[i];
                    if (rid >= 0 && rid < static_cast<int>(requests.size())) {
                        requests[rid].decodeDownReady = true;
                        requests[rid].stage = 1; // D_POST_READY
                        dPostReadyList.push_back(rid);
                    }
                }
            }
        }
    }
};

} // namespace NewSolver

int main() {
    std::cout << "=====================================================================\n";
    std::cout << "     EXACT TRUNCATION PROOF: OLD (m=64 cap) vs NEW (Dynamic vector) \n";
    std::cout << "=====================================================================\n";

    int testBatchSizes[] = {64, 65, 128, 256, 1024};

    for (int batchSize : testBatchSizes) {
        std::cout << "\n---------------------------------------------------------------------\n";
        std::cout << "[TESTING BATCH SIZE m = " << batchSize << "]\n";

        // Setup Old Solver
        OldSolver::StateTracker oldTracker;
        oldTracker.requests.resize(batchSize);
        for (int i = 0; i < batchSize; ++i) oldTracker.requests[i].rid = i;

        OldSolver::Event oldEv;
        strcpy(oldEv.stage_tag, "DEC");
        strcpy(oldEv.direction, "DOWN");
        oldEv.m = batchSize;
        int fetchCount = std::min<int>(batchSize, 64);
        for (int i = 0; i < fetchCount; ++i) oldEv.rids[i] = i;

        oldTracker.applyTransferCompletions({oldEv});

        // Setup New Solver
        NewSolver::StateTracker newTracker;
        newTracker.requests.resize(batchSize);
        for (int i = 0; i < batchSize; ++i) newTracker.requests[i].rid = i;

        NewSolver::Event newEv;
        strcpy(newEv.stage_tag, "DEC");
        strcpy(newEv.direction, "DOWN");
        newEv.m = batchSize;
        newEv.rids.resize(batchSize);
        for (int i = 0; i < batchSize; ++i) newEv.rids[i] = i;

        newTracker.applyTransferCompletions({newEv});

        // Compare Results
        int oldReadyCount = static_cast<int>(oldTracker.dPostReadyList.size());
        int newReadyCount = static_cast<int>(newTracker.dPostReadyList.size());

        std::cout << "  - Old Solver Ready Count: " << oldReadyCount << " / " << batchSize << "\n";
        std::cout << "  - New Solver Ready Count: " << newReadyCount << " / " << batchSize << "\n";

        if (batchSize > 64) {
            assert(oldReadyCount == 64);
            assert(newReadyCount == batchSize);
            std::cout << "  ===> PROVEN: Old Solver TRUNCATED " << (batchSize - 64) 
                      << " requests! New Solver preserved all " << batchSize << " requests.\n";
        } else {
            assert(oldReadyCount == batchSize);
            assert(newReadyCount == batchSize);
            std::cout << "  ===> Both solvers handle m <= 64 identically.\n";
        }
    }

    std::cout << "\n=====================================================================\n";
    std::cout << "  [EMPIRICAL PROOF COMPLETE] contest-final-v1.0 truncates at m=64!   \n";
    std::cout << "=====================================================================\n";

    return 0;
}
