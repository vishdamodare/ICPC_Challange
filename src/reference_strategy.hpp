#ifndef REFERENCE_STRATEGY_HPP
#define REFERENCE_STRATEGY_HPP

#include "strategy.hpp"

class ReferenceStrategy : public SchedulingStrategy {
private:
    int roundRobinCounter = 0;

public:
    std::vector<Task> selectTasks(const StateTracker& state, const std::vector<Task>& candidates) override;
};

#endif // REFERENCE_STRATEGY_HPP
