#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <cstdio>

static char g_simLineBuf[256];

struct RunMetrics {
    double wallMs;
    double solverCpuMs;
    double simCpuMs;
    double combinedCpuMs;
    double usPerFrame;
    double rssMb;
    long violations;
    int exitCode;
};

RunMetrics runSingle2MBenchmark(long long targetFrames) {
    SystemConfig sys;
    sys.K = 8; sys.S = 1.0; sys.num_layers = 4;
    sys.latency_in_ms = 2.0; sys.bandwidth_gbps = 1.0; sys.bytes_per_token = 125000;

    ScoringConfig sc;
    sc.SLO1 = 30.0; sc.SLO2 = 15.0; sc.tp_UB = 0.5; sc.tp_base = 0.02; sc.dist_base = 2.0;
    sc.w_tp = 0.5; sc.w_c = 0.5;

    TaskTable table;
    table.N = 2;
    table.raw_rows = {
        {1, 3.0, 10.0, 2.0, 1.0, 4.0, 1.0},
        {32, 3.0, 10.0, 2.0, 4.0, 12.0, 4.0}
    };

    int sim_to_solver[2];
    int solver_to_sim[2];

    if (pipe(sim_to_solver) < 0 || pipe(solver_to_sim) < 0) {
        std::cerr << "Pipe creation failed!\n";
        return {};
    }

    struct rusage childStartUsage;
    getrusage(RUSAGE_CHILDREN, &childStartUsage);

    struct rusage selfStartUsage;
    getrusage(RUSAGE_SELF, &selfStartUsage);

    auto startWallTime = std::chrono::high_resolution_clock::now();

    pid_t pid = fork();
    if (pid == 0) {
        dup2(sim_to_solver[0], STDIN_FILENO);
        dup2(solver_to_sim[1], STDOUT_FILENO);

        close(sim_to_solver[0]); close(sim_to_solver[1]);
        close(solver_to_sim[0]); close(solver_to_sim[1]);

        execl("./solver", "./solver", nullptr);
        exit(1);
    }

    close(sim_to_solver[0]);
    close(solver_to_sim[1]);

    FILE* out_to_solver = fdopen(sim_to_solver[1], "w");
    FILE* in_from_solver = fdopen(solver_to_sim[0], "r");

    setvbuf(out_to_solver, nullptr, _IOFBF, 65536);
    setvbuf(in_from_solver, nullptr, _IOFBF, 65536);

    fprintf(out_to_solver, "%d %.9f %.9f %.9f %lld %d\n", 
            sys.K, sys.S, sys.latency_in_ms, sys.bandwidth_gbps, sys.bytes_per_token, sys.num_layers);

    fprintf(out_to_solver, "%.9f %.9f %.9f %.9f %.9f %.9f %.9f\n",
            sc.SLO1, sc.SLO2, sc.tp_UB, sc.tp_base, sc.dist_base, sc.w_tp, sc.w_c);

    fprintf(out_to_solver, "%d\n", table.N);
    for (const auto& row : table.raw_rows) {
        fprintf(out_to_solver, "%d %.9f %.9f %.9f %.9f %.9f %.9f\n",
                row.batch_size, row.prefill_pre, row.prefill_proc, row.prefill_post,
                row.decode_pre, row.decode_proc, row.decode_post);
    }
    fflush(out_to_solver);

    long long protocolViolations = 0;
    long long malformedResponses = 0;

    for (long long f = 0; f < targetFrames; ++f) {
        double timestamp = f * 0.1;
        int rid = f % 2000;

        if (f % 4 == 0) {
            fprintf(out_to_solver, "%.1f\n1\nARR %d 4\n", timestamp, rid);
        } else if (f % 4 == 1) {
            fprintf(out_to_solver, "%.1f\n1\nTDN E D POST -1 1 %d 1.000000000\n", timestamp, rid);
        } else if (f % 4 == 2) {
            fprintf(out_to_solver, "%.1f\n1\nXDN UP 0 125000 DEC 1 %d\n", timestamp, rid);
        } else {
            fprintf(out_to_solver, "%.1f\n1\nFIN %d\n", timestamp, rid);
        }
        fflush(out_to_solver);

        int nTasks = 0;
        if (fscanf(in_from_solver, "%d", &nTasks) != 1) {
            protocolViolations++;
            break;
        }
        int ch = fgetc(in_from_solver);
        (void)ch;

        for (int i = 0; i < nTasks; ++i) {
            if (fgets(g_simLineBuf, sizeof(g_simLineBuf), in_from_solver) == nullptr) {
                malformedResponses++;
            }
        }
    }

    fprintf(out_to_solver, "END\n");
    fflush(out_to_solver);

    fclose(out_to_solver);
    fclose(in_from_solver);

    int status = 0;
    waitpid(pid, &status, 0);

    auto endWallTime = std::chrono::high_resolution_clock::now();
    double wallTimeMs = std::chrono::duration<double, std::milli>(endWallTime - startWallTime).count();

    struct rusage childEndUsage;
    getrusage(RUSAGE_CHILDREN, &childEndUsage);

    struct rusage selfEndUsage;
    getrusage(RUSAGE_SELF, &selfEndUsage);

    double childUtu = (childEndUsage.ru_utime.tv_sec - childStartUsage.ru_utime.tv_sec) * 1000.0 +
                      (childEndUsage.ru_utime.tv_usec - childStartUsage.ru_utime.tv_usec) / 1000.0;
    double childStu = (childEndUsage.ru_stime.tv_sec - childStartUsage.ru_stime.tv_sec) * 1000.0 +
                      (childEndUsage.ru_stime.tv_usec - childStartUsage.ru_stime.tv_usec) / 1000.0;
    double childCpuMs = childUtu + childStu;

    double selfUtu = (selfEndUsage.ru_utime.tv_sec - selfStartUsage.ru_utime.tv_sec) * 1000.0 +
                     (selfEndUsage.ru_utime.tv_usec - selfStartUsage.ru_utime.tv_usec) / 1000.0;
    double selfStu = (selfEndUsage.ru_stime.tv_sec - selfStartUsage.ru_stime.tv_sec) * 1000.0 +
                     (selfEndUsage.ru_stime.tv_usec - selfStartUsage.ru_stime.tv_usec) / 1000.0;
    double selfCpuMs = selfUtu + selfStu;

    RunMetrics m;
    m.wallMs = wallTimeMs;
    m.solverCpuMs = childCpuMs;
    m.simCpuMs = selfCpuMs;
    m.combinedCpuMs = childCpuMs + selfCpuMs;
    m.usPerFrame = (wallTimeMs / targetFrames * 1000.0);
    m.rssMb = static_cast<double>(childEndUsage.ru_maxrss) / (1024.0 * 1024.0);
    m.violations = protocolViolations;
    m.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return m;
}

int main() {
    int totalRuns = 30;
    long long framesPerRun = 2000000;

    std::cout << "=========================================================================================================\n";
    std::cout << "     30 REPEATED 2,000,000 FRAME REAL PROCESS IPC BENCHMARK (STABILITY CAMPAIGN)                          \n";
    std::cout << "=========================================================================================================\n";
    std::cout << std::left << std::setw(8) << "Run #"
              << std::setw(15) << "Wall (ms)"
              << std::setw(16) << "Solver CPU(ms)"
              << std::setw(14) << "Sim CPU(ms)"
              << std::setw(12) << "us/frame"
              << std::setw(12) << "Solver RSS"
              << std::setw(12) << "Violations"
              << std::setw(10) << "Exit Code\n";
    std::cout << "---------------------------------------------------------------------------------------------------------\n";

    std::vector<RunMetrics> runs(totalRuns);
    std::vector<double> wallTimesSec(totalRuns);
    std::vector<double> solverCpuSec(totalRuns);
    std::vector<double> simCpuSec(totalRuns);
    std::vector<double> combinedCpuSec(totalRuns);

    for (int r = 0; r < totalRuns; ++r) {
        runs[r] = runSingle2MBenchmark(framesPerRun);
        wallTimesSec[r] = runs[r].wallMs / 1000.0;
        solverCpuSec[r] = runs[r].solverCpuMs / 1000.0;
        simCpuSec[r] = runs[r].simCpuMs / 1000.0;
        combinedCpuSec[r] = runs[r].combinedCpuMs / 1000.0;

        std::cout << std::left << std::setw(8) << (r + 1)
                  << std::setw(15) << std::fixed << std::setprecision(2) << runs[r].wallMs
                  << std::setw(16) << runs[r].solverCpuMs
                  << std::setw(14) << runs[r].simCpuMs
                  << std::setw(12) << std::setprecision(3) << runs[r].usPerFrame
                  << std::setw(12) << std::setprecision(2) << runs[r].rssMb
                  << std::setw(12) << runs[r].violations
                  << std::setw(10) << runs[r].exitCode << std::endl;
    }

    auto calcStats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        double sum = std::accumulate(v.begin(), v.end(), 0.0);
        double mean = sum / n;
        double min_v = v.front();
        double max_v = v.back();
        double p10 = v[static_cast<size_t>(n * 0.10)];
        double p25 = v[static_cast<size_t>(n * 0.25)];
        double median = v[n / 2];
        double p75 = v[static_cast<size_t>(n * 0.75)];
        double p90 = v[static_cast<size_t>(n * 0.90)];
        double p95 = v[static_cast<size_t>(n * 0.95)];
        double p99 = v[static_cast<size_t>(n * 0.99)];
        double sq_sum = 0.0;
        for (double x : v) sq_sum += (x - mean) * (x - mean);
        double stddev = std::sqrt(sq_sum / n);
        return std::make_tuple(min_v, p10, p25, median, p75, p90, p95, p99, max_v, mean, stddev);
    };

    auto [wMin, wP10, wP25, wMed, wP75, wP90, wP95, wP99, wMax, wMean, wStd] = calcStats(wallTimesSec);
    auto [cMin, cP10, cP25, cMed, cP75, cP90, cP95, cP99, cMax, cMean, cStd] = calcStats(solverCpuSec);
    auto [sMin, sP10, sP25, sMed, sP75, sP90, sP95, sP99, sMax, sMean, sStd] = calcStats(simCpuSec);
    auto [bMin, bP10, bP25, bMed, bP75, bP90, bP95, bP99, bMax, bMean, bStd] = calcStats(combinedCpuSec);

    std::cout << "\n=========================================================================================================================\n";
    std::cout << "     30-RUN IPC BENCHMARK FULL PERCENTILE BREAKDOWN (SECONDS)\n";
    std::cout << "=========================================================================================================================\n";
    std::cout << "  Metric      Min(s)   P10(s)   P25(s)   P50(s)   P75(s)   P90(s)   P95(s)   P99(s)   Max(s)   Mean(s)  StdDev(s) Target\n";
    std::cout << "-------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << "  Wall Clock  " << std::setw(9) << std::fixed << std::setprecision(3) << wMin
              << std::setw(9) << wP10
              << std::setw(9) << wP25
              << std::setw(9) << wMed
              << std::setw(9) << wP75
              << std::setw(9) << wP90
              << std::setw(9) << wP95
              << std::setw(9) << wP99
              << std::setw(9) << wMax
              << std::setw(9) << wMean
              << std::setw(10) << wStd
              << "P50<=12.5, P95<13.5\n";
    std::cout << "  Solver CPU  " << std::setw(9) << cMin
              << std::setw(9) << cP10
              << std::setw(9) << cP25
              << std::setw(9) << cMed
              << std::setw(9) << cP75
              << std::setw(9) << cP90
              << std::setw(9) << cP95
              << std::setw(9) << cP99
              << std::setw(9) << cMax
              << std::setw(9) << cMean
              << std::setw(10) << cStd
              << "—\n";
    std::cout << "  Sim CPU     " << std::setw(9) << sMin
              << std::setw(9) << sP10
              << std::setw(9) << sP25
              << std::setw(9) << sMed
              << std::setw(9) << sP75
              << std::setw(9) << sP90
              << std::setw(9) << sP95
              << std::setw(9) << sP99
              << std::setw(9) << sMax
              << std::setw(9) << sMean
              << std::setw(10) << sStd
              << "—\n";
    std::cout << "  Comb CPU    " << std::setw(9) << bMin
              << std::setw(9) << bP10
              << std::setw(9) << bP25
              << std::setw(9) << bMed
              << std::setw(9) << bP75
              << std::setw(9) << bP90
              << std::setw(9) << bP95
              << std::setw(9) << bP99
              << std::setw(9) << bMax
              << std::setw(9) << bMean
              << std::setw(10) << bStd
              << "—\n";
    std::cout << "=========================================================================================================================\n\n";

    return 0;
}
