#ifndef ADAPTIVE_STRATEGY_HPP
#define ADAPTIVE_STRATEGY_HPP

#include "strategy.hpp"
#include "task_table.hpp"

class AdaptiveStrategy : public SchedulingStrategy {
private:
    TaskTable taskTable;
    std::vector<double> cloudEstimatedFinishTime;

public:
    explicit AdaptiveStrategy(const TaskTable& table) : taskTable(table) {}

    std::vector<Task> selectTasks(const StateTracker& state, const std::vector<Task>& candidates) override;

private:
    int computeOptimalBatchSize(TaskStep step, int availableCount, double SLO_target) const;
};

#endif // ADAPTIVE_STRATEGY_HPP
