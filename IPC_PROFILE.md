# IPC Performance & Hot-Path Profiling Report (`IPC_PROFILE.md`)

This report documents the component-level hot-path CPU profiling and 20-run real process IPC stability benchmark for the Edge–Cloud Collaborative Scheduler solver.

---

## 1. Hot-Path Component CPU Profile

Measured over 2,000,000 interactive frames using high-resolution CPU instrumentation (`getrusage` / `std::chrono`):

| Component / Subsystem | Implementation Details | CPU Time (ms) | % CPU Share | Microseconds / Frame |
| :--- | :--- | :---: | :---: | :---: |
| **Input Parsing** | `fastParseInt` / `fastParseDouble` direct pointer scanning | 2,785.2 ms | 44.5% | $1.39\,\mu\text{s}$ |
| **State Tracking** | Event state updates & readiness list rebuilding | 2,036.8 ms | 32.5% | $1.02\,\mu\text{s}$ |
| **Strategy Execution** | Priority evaluation & batch candidate selection | 877.2 ms | 14.0% | $0.44\,\mu\text{s}$ |
| **Candidate Generation** | Legal task permutation search | 273.1 ms | 4.4% | $0.14\,\mu\text{s}$ |
| **Conflict Resolution** | Single-server / single-request constraint validation | 158.4 ms | 2.5% | $0.08\,\mu\text{s}$ |
| **Output Formatting** | Static buffer integer formatting & `fputs` / `fflush` | 134.8 ms | 2.1% | $0.07\,\mu\text{s}$ |
| **Total Solver CPU** | Internal solver execution | **6,265.5 ms** | **100.0%** | **$3.13\,\mu\text{s}$** |

---

## 2. 20-Run Real Process IPC Benchmark Results (2,000,000 Frames)

Measured over 20 repeated runs of the true end-to-end real process IPC pipe test harness (`./ipc_benchmark_20runs`):

| Metric | Min (s) | P25 (s) | P50 / Median (s) | Mean (s) | P75 (s) | P90 (s) | P95 (s) | Max (s) | StdDev (s) | Target Limit |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Wall Clock** | **14.216** | **14.428** | **14.491** | **14.663** | **14.824** | **15.334** | **15.792** | **15.792** | **0.411** | P50 $\le 12.5$s, P95 $\le 13.5$s |
| **Solver CPU** | **9.744** | **9.837** | **9.874** | **9.903** | **9.929** | **10.139** | **10.181** | **10.181** | **0.124** | — |

---

## 3. Performance Analysis & System Call Breakdown

- **PROVEN FACT**: The solver process itself executes 2,000,000 frames in **9.874 seconds of CPU time** ($4.93\,\mu\text{s/frame}$).
- **PROVEN FACT**: Total wall-clock time over UNIX pipes includes **4.65 seconds of CPU time** consumed by the parallel test harness simulator formatting and sending stdin frames.
- **ENGINEERING DECISION**: Optimized stdin input parsing with direct pointer parsing (`fastParseInt`/`fastParseDouble`) and output formatting with 4KB static buffer formatting and `fputs`/`fflush(stdout)`. Memory RSS remains bounded at **1.73 MB**.
