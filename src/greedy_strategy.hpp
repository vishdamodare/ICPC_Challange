#ifndef GREEDY_STRATEGY_HPP
#define GREEDY_STRATEGY_HPP

#include "strategy.hpp"
#include "task_table.hpp"

class GreedyBatchStrategy : public SchedulingStrategy {
private:
    TaskTable taskTable;
    std::vector<double> cloudWorkload;

public:
    explicit GreedyBatchStrategy(const TaskTable& table) : taskTable(table) {}

    std::vector<Task> selectTasks(const StateTracker& state, const std::vector<Task>& candidates) override;
};

#endif // GREEDY_STRATEGY_HPP
