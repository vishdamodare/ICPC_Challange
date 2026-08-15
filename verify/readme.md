# Local verification kit

## 1. Compile exactly like Codeforces
    g++ -std=c++20 -O2 submission.cpp -o solver

## 2. Correctness vs the official worked Example 1
    python3 validate_example1.py
Expect: `violation: None`, TDR=30.0, token time=[45.0] — must match problem.md exactly.
Edit the file's `solver_baseline` path to point at your `solver` binary if needed.

## 3. Timing / memory under load (edit paths inside stress_measure.py, not included
   here — ask me to regenerate it against your actual solver binary and I will,
   with parameters sized to your worst-case R/K/frame-count).

## 4. Protocol-violation sweep
    python3 ab_run.py 3000 <any_seed> baseline
Change the solver path inside ab_run.py to your binary. Look for any
`"violation": <non-null>` in the output -- that means a real bug (illegal
task, malformed output, stuck state) was found, not just a low score.
A score of 0 with `violation: null` is NOT a bug -- it's the scoring
formula's dist_base=0 cliff (see problem.md), which is expected behavior
on some test parameterizations, not a sign of a broken submission.

## 5. IMPORTANT CAVEAT
This harness is a self-built, best-effort re-implementation of the protocol
for A/B and regression testing -- it is NOT the Codeforces judge. It's good
for: (a) catching protocol violations / stuck states / crashes before you
burn a submission, (b) confirming two versions of your code behave
identically. It is NOT good for: predicting your actual numeric score,
since it doesn't have the real hidden test distribution.