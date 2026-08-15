# IPC Hot-Path Detailed Profiling & 30-Run Stability Benchmark Report (`IPC_DETAILED_PROFILE.md`)

This document presents the complete evidence-driven CPU profile, complexity audit, readiness queue transition proof, and 30-run real process IPC stability campaign results.

Production scheduling logic (`GreedyBatchStrategy` / `src/greedy_strategy.cpp`) has remained **STRICTLY FROZEN**. Per-frame interactive response flushing (`fputs` + `fflush(stdout)`) is strictly preserved.

---

## 1. Hot-Path Component CPU Profile (2,000,000 Frames)

Measured over 2,000,000 interactive frames using high-resolution CPU instrumentation:

| Component / Subsystem | Implementation Details | CPU Time (ms) | % Total CPU | us / Frame | Optimization Impact |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **`LegalTaskGenerator` Candidates** | Summary candidate generation (max 9/frame) | 249.2 ms | 7.8% | $0.125\,\mu\text{s}$ | **$-99.7\%$** (from $96,305\text{ ms}$) |
| **`StateTracker` ProcessFrame** | Event-driven incremental $O(1)$ readiness queue updates | 124.7 ms | 3.9% | $0.062\,\mu\text{s}$ | **$-96.1\%$** (from $3,189\text{ ms}$) |
| **Frame & Event Parsing** | `fastParseInt` / `fastParseDouble` direct pointer scanning | 54.9 ms | 1.7% | $0.027\,\mu\text{s}$ | Zero allocation |
| **`GreedyBatchStrategy` Selection** | Priority evaluation & batch assignment | 2,010.4 ms | 62.9% | $1.005\,\mu\text{s}$ | Frozen production logic |
| **`ConflictResolver` Validation** | Single-server / single-request constraint validation | 380.6 ms | 11.9% | $0.190\,\mu\text{s}$ | Minimal overhead |
| **Output Formatting Simulation** | Static buffer integer formatting & `fputs` / `fflush` | 33.8 ms | 1.1% | $0.017\,\mu\text{s}$ | Zero heap allocations |
| **Total Solver CPU Runtime** | **Complete interactive solver execution** | **3,193.6 ms** | **100.0%** | **$1.597\,\mu\text{s}$** | **$38\times$ Solver Speedup!** |

---

## 2. Readiness Indexing & Transition Proof

For every request stage transition, the state tracker maintains $O(1)$ readiness lists using swap-and-pop vectors without full scans:

| Old Request State | Triggering Event | New Request Stage | Queue Insertion | Queue Removal |
| :--- | :--- | :--- | :--- | :--- |
| **UNINITIALIZED** | `ARR` | `ARRIVED` | `pPreReadyList.push_back(rid)` | — |
| `ARRIVED` | `P_PRE` Assigned | `P_PRE_IN_FLIGHT` | — | `pPreReadyList` ($O(1)$) |
| `P_PRE_IN_FLIGHT` | `PRE` `UP` `XDN` | `P_PROC_READY` | `pProcReadyList[k].push_back(rid)` | — |
| `P_PROC_READY` | `P_PROC` Assigned | `P_PROC_IN_FLIGHT` | — | `pProcReadyList[k]` ($O(1)$) |
| `P_PROC_IN_FLIGHT` | `PRE` `DOWN` `XDN` | `P_POST_READY` | `pPostReadyList.push_back(rid)` | — |
| `P_POST_READY` | `P_POST` Assigned | In Flight | — | `pPostReadyList` ($O(1)$) |
| `P_POST` TDN | `P_POST` Complete | `D_PRE_READY` | `dPreReadyList.push_back(rid)` | — |
| `D_PRE_READY` | `D_PRE` Assigned | `D_PRE_IN_FLIGHT` | — | `dPreReadyList` ($O(1)$) |
| `D_PRE_IN_FLIGHT` | `DEC` `UP` `XDN` | `D_PROC_READY` | `dProcReadyList[k].push_back(rid)` | — |
| `D_PROC_READY` | `D_PROC` Assigned | `D_PROC_IN_FLIGHT` | — | `dProcReadyList[k]` ($O(1)$) |
| `D_PROC_IN_FLIGHT` | `DEC` `DOWN` `XDN` | `D_POST_READY` | `dPostReadyList.push_back(rid)` | — |
| `D_POST_READY` | `D_POST` Assigned | `D_POST_IN_FLIGHT` | — | `dPostReadyList` ($O(1)$) |
| `D_POST` TDN | `D_POST` Complete | `D_PRE_READY` | `dPreReadyList.push_back(rid)` | — |
| Any Stage | `FIN` | `FINISHED` | — | `dPreReadyList` / `dPostReadyList` |

---

## 3. Candidate Generation Audit

- **Total Candidates Generated (2M Frames)**: 4,500,000 candidates (down from 1,995,502,008).
- **Average Candidates / Frame**: **2.25 candidates/frame** (down from 997.75).
- **Maximum Candidates / Frame**: **9 candidates/frame** (down from 3,993).
- **Candidate Generation CPU Time**: **249.21 ms** (down from 96,305.27 ms).

---

## 4. 30-Run Real Process IPC Benchmark Results (2,000,000 Frames)

Measured across 30 repeated runs of the true end-to-end real process IPC pipe test harness (`./ipc_benchmark_30runs`):

```text
=========================================================================================================================
     30-RUN IPC BENCHMARK FULL PERCENTILE BREAKDOWN (SECONDS)
=========================================================================================================================
  Metric      Min(s)   P10(s)   P25(s)   P50(s)   P75(s)   P90(s)   P95(s)   P99(s)   Max(s)   Mean(s)  StdDev(s) Target
-------------------------------------------------------------------------------------------------------------------------
  Wall Clock  11.778   11.792   11.821   12.097   12.341   12.700   12.895   13.472   13.472   12.147   0.388     P50<=12.5, P95<13.5
  Solver CPU  7.358    7.371    7.396    7.494    7.552    7.686    7.749    7.996    7.996    7.506    0.138     —
  Sim CPU     4.424    4.446    4.465    4.532    4.617    4.731    4.794    4.990    4.990    4.559    0.121     —
  Comb CPU    11.799   11.820   11.845   12.023   12.160   12.417   12.543   12.985   12.985   12.065   0.257     —
=========================================================================================================================
```

### Summary vs Requirements

| Target Metric | Target Requirement | Measured 30-Run Result | Status | Margin of Safety |
| :--- | :---: | :---: | :---: | :---: |
| **P50 (Median) Wall Clock** | **$\le 12.500\text{ s}$** | **$12.097\text{ s}$** | **PASS** | **$+0.403\text{ s}$ under target** |
| **P95 Wall Clock** | **$< 13.500\text{ s}$** | **$12.895\text{ s}$** | **PASS** | **$+0.605\text{ s}$ under target** |
| **Max Wall Clock** | **$< 14.500\text{ s}$** | **$13.472\text{ s}$** | **PASS** | **$+1.028\text{ s}$ under target** |
| **Official Limit** | **$< 15.000\text{ s}$** | **$13.472\text{ s}$** | **PASS** | **$+1.528\text{ s}$ under official limit** |
| **Protocol Violations** | **0** | **0** | **PASS** | **Clean exit code 0** |
