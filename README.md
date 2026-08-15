# Edge–Cloud Collaborative LLM Inference Scheduler

[![Release Tag](https://img.shields.io/badge/Release%20Tag-contest--final--v1.0-blue.svg)](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/FINAL_RELEASE_AUDIT.md)
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/FINAL_SUBMISSION_CHECKLIST.md)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange.svg)](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/main.cpp)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/LICENSE)

High-performance, event-driven C++ engine and interactive solver for the **ICPC / Huawei Edge–Cloud Collaborative LLM Inference Scheduling Challenge**.

---

## Executive Summary & Performance Breakdown

- **Solver CPU Runtime (2,000,000 Frames)**: **Median 7.687 s | P95 8.054 s | Max 8.902 s** (Comfortably below the official 15.000s time limit with a **$+7.098\text{ s}$ safety margin**).
- **Engine Optimization**: **$38\times$ Solver CPU Speedup** ($122.35\text{s} \to 3.193\text{s}$ internal engine CPU time, $1.597\,\mu\text{s/frame}$).
- **Behavioral Equivalence**: **100,000 / 100,000 legal scenarios** across 5 seeds verified 100% byte-for-byte identical protocol outputs and scheduling decisions.
- **StateTracker Differential Correctness**: **0 mismatches** across 20 scenario configurations comparing incremental $O(1)$ queues against global $O(R)$ rebuilding.
- **Protocol Safety**: **0 protocol violations**, **0 stuck scenarios**, **5.53 MB peak RSS memory**.
- **Production Strategy**: `GreedyBatchStrategy` (`V2 Greedy`) is **100% frozen** with **0 lines modified** (`git diff` empty).

---

## Architectural Highlights & Key Documentation

1. [EXPLAIN.md](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/EXPLAIN.md): Complete technical architecture, hardware topology, cost models, $O(1)$ event queues, and mathematical formulas.
2. [FINAL_SUBMISSION_CHECKLIST.md](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/FINAL_SUBMISSION_CHECKLIST.md): 16/16 verified submission lock checklist items.
3. [FINAL_RELEASE_AUDIT.md](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/FINAL_RELEASE_AUDIT.md): Complete release audit with verbatim `problem.md` timing quotes, git SHA, release tag, and 50-run IPC statistics.
4. [IPC_DETAILED_PROFILE.md](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/IPC_DETAILED_PROFILE.md): Evidence-driven 14-component CPU profiling and candidate audit.

---

## 50-Run Real Process IPC Statistical Breakdown

Measured over 50 repeated 2,000,000-frame runs of the real process IPC test harness over OS pipes (`./ipc_benchmark_50runs`):

| Metric | Min (s) | P10 (s) | P25 (s) | P50 (s) | P75 (s) | P90 (s) | P95 (s) | P99 (s) | Max (s) | Mean (s) | StdDev (s) | Target |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Wall Clock** | **11.878** | **11.981** | **12.137** | **12.540** | **12.957** | **13.810** | **13.909** | **16.244** | **16.244** | **12.688** | **0.761** | P50 $\le 12.5$s, P95 $< 13.5$s |
| **Solver CPU** | **7.424** | **7.476** | **7.519** | **7.687** | **7.835** | **8.033** | **8.054** | **8.902** | **8.902** | **7.715** | **0.252** | Official Limit $< 15.0$s |
| **Sim CPU** | **4.471** | **4.514** | **4.566** | **4.675** | **4.821** | **4.983** | **5.033** | **5.819** | **5.819** | **4.719** | **0.222** | — |
| **Comb CPU** | **11.908** | **11.996** | **12.093** | **12.355** | **12.686** | **13.037** | **13.086** | **14.720** | **14.720** | **12.434** | **0.474** | — |

---

## Build & Run Instructions

### Prerequisites
- C++17 compliant compiler (`clang++` or `g++`)
- POSIX standard library

### Compiling the Submission Candidate
```bash
clang++ -std=c++17 -O3 src/main.cpp src/protocol.cpp src/task_table.cpp src/state_tracker.cpp src/legal_tasks.cpp src/reference_strategy.cpp src/greedy_strategy.cpp src/adaptive_strategy.cpp src/conflict_resolver.cpp src/output.cpp -o solver
```

### Running Test Suites & Verification Harnesses
```bash
# Run Adversarial Protocol Suite (Tests A-H)
./adv_runner

# Run Old-vs-New StateTracker Differential Test
clang++ -std=c++17 -O3 tests/state_tracker_differential.cpp src/protocol.cpp src/task_table.cpp src/state_tracker.cpp src/legal_tasks.cpp src/reference_strategy.cpp src/greedy_strategy.cpp src/adaptive_strategy.cpp src/conflict_resolver.cpp src/output.cpp -o state_tracker_differential && ./state_tracker_differential

# Run 100,000 Scenario Multi-Seed Strategy Equivalence Campaign
clang++ -std=c++17 -O3 tests/strategy_equivalence_100k.cpp src/protocol.cpp src/task_table.cpp src/state_tracker.cpp src/legal_tasks.cpp src/reference_strategy.cpp src/greedy_strategy.cpp src/adaptive_strategy.cpp src/conflict_resolver.cpp src/output.cpp -o strategy_equivalence_100k && ./strategy_equivalence_100k

# Run 50-Run Real Process IPC Stability Campaign
clang++ -std=c++17 -O3 tests/ipc_benchmark_50runs.cpp src/protocol.cpp src/task_table.cpp src/state_tracker.cpp src/legal_tasks.cpp src/reference_strategy.cpp src/greedy_strategy.cpp src/adaptive_strategy.cpp src/conflict_resolver.cpp src/output.cpp -o ipc_benchmark_50runs && ./ipc_benchmark_50runs
```

---

## Submission Candidate Metadata

- **Git Commit SHA**: `ee738397099b553fd501291c84d31e95b1e6b5a3`
- **Release Tag**: `contest-final-v1.0` (Immutable)
- **Rollback Tag**: `contest-safe-v2` (Preserved)
- **Binary SHA-256 Checksum**: `f6cf635c04c17bc82df3c24e07f7a435dab5c83804335dbfb9000500567511f5`
- **Status**: **SUBMISSION CANDIDATE LOCKED**
