# 100,000-Scenario Evaluation Campaign Report (`EVALUATION_RESULTS.md`)

This report presents the statistical evaluation of the Edge–Cloud Collaborative Scheduler across **100,000 procedurally generated legal scenarios** generated across **5 independent random seeds** (`42`, `123`, `2026`, `8675309`, `314159`).

---

## 1. Multi-Seed Split Distribution

- **Training Split (60%)**: 60,000 scenarios
- **Untouched Validation Split (20%)**: 20,000 scenarios
- **Untouched Holdout Split (20%)**: 20,000 scenarios

---

## 2. Statistical Results Summary Across Splits

### A. Training Split (60,000 Scenarios)

| Metric / Percentile | V1 Reference | V2 Greedy (Safe Baseline) | V3 Candidate | Candidate Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Mean Score** | **498.6** | **498.6** | **498.6** | `+0.0` |
| **Minimum Score** | `0.0` | `0.0` | `0.0` | `+0.0` |
| **P1 Score** | `10.3` | `10.3` | `10.3` | `+0.0` |
| **P5 Score** | `51.0` | `51.0` | `51.0` | `+0.0` |
| **P10 Score** | `102.3` | `102.3` | `102.3` | `+0.0` |
| **P25 Score** | `246.7` | `246.7` | `246.7` | `+0.0` |
| **P50 (Median) Score** | `496.3` | `496.3` | `496.3` | `+0.0` |
| **P75 Score** | `750.5` | `750.5` | `750.5` | `+0.0` |
| **P90 Score** | `899.2` | `899.2` | `899.2` | `+0.0` |
| **P95 Score** | `949.6` | `949.6` | `949.6` | `+0.0` |
| **Mean TDR (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Mean TPOT (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Protocol Violations** | `0` | `0` | `0` | `0` |
| **Stuck Cases** | `0` | `0` | `0` | `0` |

---

### B. Untouched Validation Split (20,000 Scenarios)

| Metric / Percentile | V1 Reference | V2 Greedy (Safe Baseline) | V3 Candidate | Candidate Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Mean Score** | **499.0** | **499.0** | **499.0** | `+0.0` |
| **Minimum Score** | `0.0` | `0.0` | `0.0` | `+0.0` |
| **P1 Score** | `9.9` | `9.9` | `9.9` | `+0.0` |
| **P5 Score** | `50.2` | `50.2` | `50.2` | `+0.0` |
| **P10 Score** | `100.6` | `100.6` | `100.6` | `+0.0` |
| **P25 Score** | `249.1` | `249.1` | `249.1` | `+0.0` |
| **P50 (Median) Score** | `495.6` | `495.6` | `495.6` | `+0.0` |
| **P75 Score** | `748.5` | `748.5` | `748.5` | `+0.0` |
| **P90 Score** | `898.3` | `898.3` | `898.3` | `+0.0` |
| **P95 Score** | `950.2` | `950.2` | `950.2` | `+0.0` |
| **Mean TDR (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Mean TPOT (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Protocol Violations** | `0` | `0` | `0` | `0` |
| **Stuck Cases** | `0` | `0` | `0` | `0` |

---

### C. Untouched Holdout Split (20,000 Scenarios)

| Metric / Percentile | V1 Reference | V2 Greedy (Safe Baseline) | V3 Candidate | Candidate Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Mean Score** | **499.7** | **499.7** | **499.7** | `+0.0` |
| **Minimum Score** | `0.0` | `0.0` | `0.0` | `+0.0` |
| **P1 Score** | `9.5` | `9.5` | `9.5` | `+0.0` |
| **P5 Score** | `49.3` | `49.3` | `49.3` | `+0.0` |
| **P10 Score** | `99.3` | `99.3` | `99.3` | `+0.0` |
| **P25 Score** | `248.9` | `248.9` | `248.9` | `+0.0` |
| **P50 (Median) Score** | `501.9` | `501.9` | `501.9` | `+0.0` |
| **P75 Score** | `751.7` | `751.7` | `751.7` | `+0.0` |
| **P90 Score** | `899.2` | `899.2` | `899.2` | `+0.0` |
| **P95 Score** | `949.2` | `949.2` | `949.2` | `+0.0` |
| **Mean TDR (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Mean TPOT (ms)** | `0.0` | `0.0` | `0.0` | `0.0 ms` |
| **Protocol Violations** | `0` | `0` | `0` | `0` |
| **Stuck Cases** | `0` | `0` | `0` | `0` |

---

## 3. Regression Breakdown vs V2 Greedy Baseline

Across all 100,000 scenarios evaluated:
- **Improved**: `0`
- **Unchanged**: `100,000`
- **Regressed**: `0`
- **Catastrophic Regressions (>50 pts)**: `0`
- **Catastrophic Regressions (>100 pts)**: `0`
- **Catastrophic Regressions (>250 pts)**: `0`
- **Catastrophic Regressions (>500 pts)**: `0`
- **Catastrophic Regressions (>750 pts)**: `0`

---

## 4. 10-Run IPC Real Process Stability Benchmark

Measured over 10 repeated executions of the 2,000,000 frame real process IPC pipe wall-clock benchmark (`./ipc_stability_runner`):

| Metric | Min | Median | Mean | Max | Std Dev | Contest Requirement |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Wall Clock (s)** | **14.477 s** | **14.819 s** | **15.271 s** | **18.221 s** | **1.107 s** | **$< 15.000\text{ s}$** |
| **Solver CPU (s)** | **9.850 s** | **9.939 s** | **10.068 s** | **10.986 s** | **0.320 s** | — |
