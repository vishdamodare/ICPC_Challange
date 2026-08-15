#ifndef CONFLICT_RESOLVER_HPP
#define CONFLICT_RESOLVER_HPP

#include "state_tracker.hpp"
#include "task.hpp"
#include <vector>

class ConflictResolver {
public:
    static std::vector<Task> resolveConflicts(const StateTracker& state, const std::vector<Task>& proposedTasks);
};

#endif // CONFLICT_RESOLVER_HPP
