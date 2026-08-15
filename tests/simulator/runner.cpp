#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>

int main() {
    int sim_to_solver[2];
    int solver_to_sim[2];

    if (pipe(sim_to_solver) < 0 || pipe(solver_to_sim) < 0) {
        std::cerr << "Failed to create pipes\n";
        return 1;
    }

    pid_t pid_sim = fork();
    if (pid_sim == 0) {
        // Child 1: Sim (reads from solver_to_sim[0], writes to sim_to_solver[1])
        dup2(solver_to_sim[0], STDIN_FILENO);
        dup2(sim_to_solver[1], STDOUT_FILENO);

        close(sim_to_solver[0]);
        close(sim_to_solver[1]);
        close(solver_to_sim[0]);
        close(solver_to_sim[1]);

        execl("./sim_main", "./sim_main", nullptr);
        std::cerr << "Failed to exec sim_main\n";
        exit(1);
    }

    pid_t pid_solver = fork();
    if (pid_solver == 0) {
        // Child 2: Solver (reads from sim_to_solver[0], writes to solver_to_sim[1])
        dup2(sim_to_solver[0], STDIN_FILENO);
        dup2(solver_to_sim[1], STDOUT_FILENO);

        close(sim_to_solver[0]);
        close(sim_to_solver[1]);
        close(solver_to_sim[0]);
        close(solver_to_sim[1]);

        execl("./solver", "./solver", nullptr);
        std::cerr << "Failed to exec solver\n";
        exit(1);
    }

    // Parent
    close(sim_to_solver[0]);
    close(sim_to_solver[1]);
    close(solver_to_sim[0]);
    close(solver_to_sim[1]);

    int status1 = 0, status2 = 0;
    waitpid(pid_sim, &status1, 0);
    waitpid(pid_solver, &status2, 0);

    std::cout << "[SIMULATOR TEST RUNNER] Simulation completed successfully!\n";
    std::cout << "  sim_main exit code: " << WEXITSTATUS(status1) << "\n";
    std::cout << "  solver exit code:   " << WEXITSTATUS(status2) << "\n";

    return 0;
}
