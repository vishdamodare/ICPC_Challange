# Final Submission Checklist (`FINAL_SUBMISSION_CHECKLIST.md`)

This checklist verifies the completion of all requirements for the Edge–Cloud Collaborative Scheduler submission candidate.

---

## Submission Lock Verification Matrix

- [x] **Strategy Frozen**: `src/greedy_strategy.cpp` has **0 lines modified** relative to frozen baseline.
- [x] **Rollback Available**: Git tag `contest-safe-v2` is fully intact and verified.
- [x] **Final Commit Recorded**: Commit SHA `ee738397099b553fd501291c84d31e95b1e6b5a3` recorded.
- [x] **Immutable Release Tag Created**: Tag `contest-final-v1.0` created and verified.
- [x] **Release Binary Rebuilt**: Clean build compiled with `clang++ -std=c++17 -O3`.
- [x] **Binary Checksum Recorded**: SHA-256 `f6cf635c04c17bc82df3c24e07f7a435dab5c83804335dbfb9000500567511f5`.
- [x] **StateTracker Differential = 0**: 0 state mismatches across 20 scenario configurations.
- [x] **100k Behavioral Equivalence = 100%**: 100,000 / 100,000 scenarios across 5 seeds produced 100% byte-for-byte identical protocol output and decisions.
- [x] **Adversarial Suite = 8/8 PASS**: Comprehensive adversarial protocol tests A–H passed.
- [x] **Event-Order Invariance = PASS**: Differential event-order test verified 100% state match under scrambled input lines.
- [x] **Startup Desync Test = PASS**: Unified C stdio stream parser handles $>15\text{ KB}$ header without desynchronization.
- [x] **Protocol Violations = 0**: Zero protocol syntax or framing violations detected across all test suites.
- [x] **Stuck Scenarios = 0**: Zero stuck scenarios or unhandled requests.
- [x] **Memory Bounded**: Peak RSS is **5.53 MB** (well below the limit).
- [x] **Official Timing Wording Documented**: Verbatim quotes from `problem.md` documented alongside both Solver CPU (**7.687s median / 8.054s P95 / 8.902s Max**) and Harness Wall Clock (**12.540s median / 13.909s P95 / 16.244s Max**).
- [x] **Final Performance Results Documented**: 50-run IPC statistical percentile breakdown compiled in `FINAL_RELEASE_AUDIT.md`.

---

## Submission Summary & Lock Confirmation

- **Release Tag**: `contest-final-v1.0`
- **Target SHA**: `ee738397099b553fd501291c84d31e95b1e6b5a3`
- **Binary SHA-256**: `f6cf635c04c17bc82df3c24e07f7a435dab5c83804335dbfb9000500567511f5`
- **Status**: **SUBMISSION CANDIDATE LOCKED**
