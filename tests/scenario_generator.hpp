#ifndef SCENARIO_GENERATOR_HPP
#define SCENARIO_GENERATOR_HPP

#include "../src/protocol.hpp"
#include "../src/task_table.hpp"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

struct BenchmarkRequest {
    int rid;
    int Lin;
    int Lout;
    double arrivalTime;
};

enum class ArrivalPattern {
    SIMULTANEOUS,
    EVEN,
    BURST,
    SPARSE,
    MIXED
};

struct Scenario {
    int seed;
    SystemConfig sys;
    ScoringConfig sc;
    TaskTable table;
    std::vector<BenchmarkRequest> requests;
    ArrivalPattern pattern;
};

class ScenarioGenerator {
public:
    static Scenario generateScenario(uint32_t seed) {
        std::mt19937 rng(seed);
        Scenario s;
        s.seed = static_cast<int>(seed);

        // Hardware configs
        std::vector<int> K_options = {1, 2, 4, 8};
        s.sys.K = K_options[rng() % K_options.size()];
        
        std::uniform_real_distribution<double> dist_S(1.0, 10.0);
        s.sys.S = dist_S(rng);

        std::vector<int> layers_options = {1, 2, 4, 8, 16, 32};
        s.sys.num_layers = layers_options[rng() % layers_options.size()];

        std::vector<double> lat_options = {0.5, 2.0, 5.0, 10.0};
        s.sys.latency_in_ms = lat_options[rng() % lat_options.size()];

        std::vector<double> bw_options = {0.5, 1.0, 4.0, 10.0};
        s.sys.bandwidth_gbps = bw_options[rng() % bw_options.size()];

        s.sys.bytes_per_token = 125000;

        // Scoring configs
        std::uniform_real_distribution<double> dist_w(0.0, 1.0);
        s.sc.w_tp = dist_w(rng);
        s.sc.w_c = 1.0 - s.sc.w_tp;

        std::uniform_real_distribution<double> dist_slo1(20.0, 200.0);
        std::uniform_real_distribution<double> dist_slo2(5.0, 50.0);
        s.sc.SLO1 = dist_slo1(rng);
        s.sc.SLO2 = dist_slo2(rng);
        s.sc.tp_UB = 0.5;
        s.sc.tp_base = 0.02;

        std::vector<double> dist_base_opts = {0.0, 1.0, 2.5, 5.0};
        s.sc.dist_base = dist_base_opts[rng() % dist_base_opts.size()];

        // Task Table
        s.table.N = 3;
        s.table.raw_rows = {
            {1,  1.0 + dist_S(rng)*0.2, 5.0 + s.sys.num_layers*1.0, 1.0, 0.5, 2.0, 0.5},
            {16, 2.0 + dist_S(rng)*0.4, 8.0 + s.sys.num_layers*1.5, 1.5, 1.0, 4.0, 1.0},
            {64, 4.0 + dist_S(rng)*0.8, 15.0 + s.sys.num_layers*2.5, 2.5, 2.0, 8.0, 2.0}
        };

        // Requests & Arrival Patterns
        std::vector<int> R_options = {2, 5, 10, 25, 50, 100, 250, 500};
        int R = R_options[rng() % R_options.size()];

        std::vector<ArrivalPattern> pattern_opts = {
            ArrivalPattern::SIMULTANEOUS,
            ArrivalPattern::EVEN,
            ArrivalPattern::BURST,
            ArrivalPattern::SPARSE,
            ArrivalPattern::MIXED
        };
        s.pattern = pattern_opts[rng() % pattern_opts.size()];

        std::uniform_int_distribution<int> dist_Lin(1, 1024);
        std::uniform_int_distribution<int> dist_Lout(1, 128);

        s.requests.resize(R);
        double currTime = 0.0;

        for (int i = 0; i < R; ++i) {
            s.requests[i].rid = i;
            s.requests[i].Lin = dist_Lin(rng);
            s.requests[i].Lout = dist_Lout(rng);

            switch (s.pattern) {
                case ArrivalPattern::SIMULTANEOUS:
                    s.requests[i].arrivalTime = 0.0;
                    break;
                case ArrivalPattern::EVEN:
                    s.requests[i].arrivalTime = i * 2.0;
                    break;
                case ArrivalPattern::BURST:
                    if (i % 10 == 0) currTime += 20.0;
                    s.requests[i].arrivalTime = currTime;
                    break;
                case ArrivalPattern::SPARSE:
                    s.requests[i].arrivalTime = i * 15.0;
                    break;
                case ArrivalPattern::MIXED:
                    if (rng() % 3 == 0) currTime += 10.0;
                    s.requests[i].arrivalTime = currTime + (rng() % 5) * 0.5;
                    break;
            }
        }

        return s;
    }
};

#endif // SCENARIO_GENERATOR_HPP
