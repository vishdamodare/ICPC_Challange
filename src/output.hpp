#ifndef OUTPUT_HPP
#define OUTPUT_HPP

#include "task.hpp"
#include <vector>
#include <iostream>

class OutputWriter {
public:
    static void writeAssignments(std::ostream& out, const std::vector<Task>& tasks);
};

#endif // OUTPUT_HPP
