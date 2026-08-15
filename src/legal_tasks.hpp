#ifndef LEGAL_TASKS_HPP
#define LEGAL_TASKS_HPP

#include "state_tracker.hpp"
#include "task.hpp"
#include <vector>

class LegalTaskGenerator {
public:
    static std::vector<Task> generateCandidates(const StateTracker& state);
};

#endif // LEGAL_TASKS_HPP
