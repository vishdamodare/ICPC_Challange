#ifndef INTERACTOR_SIMULATOR_HPP
#define INTERACTOR_SIMULATOR_HPP

#include "../../src/protocol.hpp"
#include "../../src/task_table.hpp"
#include <vector>
#include <string>
#include <map>

struct SimRequest {
    int rid;
    double arrivalTime;
    int Lin;
    int Lout;
};

struct SimResult {
    bool success = true;
    std::string errorMessage;
    double totalTime = 0.0;
    double score = 0.0;
    double tp = 0.0;
    double tdr = 0.0;
    double tpot = 0.0;
};

class InteractorSimulator {
public:
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<SimRequest> requests;

    SimResult runSimulation(const std::string& solverCmd);
};

#endif // INTERACTOR_SIMULATOR_HPP
