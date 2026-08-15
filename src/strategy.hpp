#ifndef STRATEGY_HPP
#define STRATEGY_HPP

#include "state_tracker.hpp"
#include "task.hpp"
#include <vector>

class SchedulingStrategy {
public:
    virtual ~SchedulingStrategy() = default;
    virtual std::vector<Task> selectTasks(const StateTracker& state, const std::vector<Task>& candidates) = 0;
};

#endif // STRATEGY_HPP
