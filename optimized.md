Edge–Cloud Collaborative Scheduler — Optimization & Winning Plan

Purpose

This document is the execution plan for improving the current contest submission without sacrificing protocol correctness.

The current production baseline is V2 Greedy. It has passed the locally available correctness tests and a true 2,000,000-frame process-IPC benchmark, but the latest measured IPC wall time is approximately 14.682 s, leaving only about 0.318 s under the official 15 s limit.

The objective is:

Preserve a known-good fallback.

Improve expected score on the frozen final tests.

Avoid catastrophic score regressions.

Increase the IPC performance margin.

Validate every candidate on large, unseen-style scenario sets before replacing V2.

Do not blindly add heuristics. Data must decide whether a change is worth merging.

0. Non-Negotiable Safety Rule

Before any experiment:

Create a permanent safe branch/tag containing the currently validated V2 implementation.

Never modify the safe branch during experimentation.

Every candidate must be reversible.

Never replace V2 merely because a small benchmark improves.

Any protocol violation, stuck state, malformed output, or timeout is an immediate rejection.

Recommended safety point:

contest-safe-v2

1. Current Baseline

Current production strategy:

V2 Greedy / maximum batching

Known validation:

Worked example: PASS

Adversarial suite: 8/8 PASS

Event-order invariance: PASS

Startup stream-desynchronization regression: PASS

Score benchmark matrix: PASS

2M internal engine benchmark: approximately 6 s CPU

True 2M process IPC benchmark: approximately 14.682 s wall

Protocol violations: 0

Exit code: 0

Memory: approximately 1.75 MB

These results are evidence, not proof of final-test performance. The official frozen final tests remain unknown.

2. Separate Correctness, Strategy, and Performance

Work in this order:

CORRECTNESS
    ↓
BASELINE MEASUREMENT
    ↓
STRATEGY RESEARCH
    ↓
UNSEEN VALIDATION
    ↓
HOT-PATH PERFORMANCE
    ↓
FINAL REGRESSION
    ↓
SUBMISSION

Do not change protocol/state semantics while simultaneously tuning strategy unless the change is isolated and independently tested.

3. Maintain Three Versions

SAFE

The current V2 Greedy implementation.

CANDIDATE

The experimental strategy.

REFERENCE

The V1/reference scheduler used for diagnostics.

Every experiment should report:

V1 vs V2 vs Candidate

4. Build a Serious Offline Scenario Generator

The generator must follow the actual specification.

Vary:

Hardware/system

K: 1,2,4,8
S: 1..10
latency: low/medium/high
bandwidth: low/medium/high
bytes_per_token: small/medium/large
num_layers: 1,2,4,8,16,32,64

Requests

Include:

R: 1,2,3,4,5,8,10,25,50,100,500,1000,2000
Lin: 1..4096
Lout: 1..512

Include short/long input-output combinations.

Arrival patterns

simultaneous
even
burst
mixed
sparse
long idle periods

Scoring

Explicitly cover:

w_tp = 1
w_c = 1
balanced weights
dist_base = 0
dist_base > 0
tight SLO1/SLO2
loose SLO1/SLO2

Task-time tables

Generate diverse legal curves for all six task types, respecting the specification and its interpolation rules.

Do not optimize against only one task-time table.

5. Use Training / Validation / Holdout Splits

Use at least 100,000 scenarios for serious policy research.

Recommended:

60% training
20% validation
20% holdout

Use multiple random seeds, for example:

42
123
2026
8675309
314159

Never tune directly against holdout results.

6. Metrics Every Policy Must Report

For every candidate:

mean score
median score
minimum
5th percentile
10th percentile
25th percentile
75th percentile
95th percentile

mean throughput component
mean waiting component

mean TDR
mean TPOT
worst TDR
worst TPOT

protocol violations
stuck tests
timeouts

Against V2, also report:

improved scenarios
unchanged scenarios
regressed scenarios

regression > 50
regression > 100
regression > 250
regression > 500
regression > 750

A small mean improvement is not enough if it hides catastrophic regressions.

7. Candidate Acceptance Criteria

A candidate is production-eligible only if:

Correctness

0 protocol violations
0 malformed outputs
0 stuck states
0 crashes
0 unexplained EOF behavior

Runtime

Must remain safely below:

15 seconds

Target approximately:

<= 13.5 seconds

if achievable.

Robustness

Reject candidates with unexplained catastrophic regressions, especially:

>500 points

or repeated large regressions.

Statistical improvement

Prefer candidates that improve mean score while preserving or improving lower percentiles.

8. Investigate D PRE First

D PRE is a major strategy lever because:

Edge D PRE is serialized
Cloud D PROC can execute in parallel

Therefore the objective is not simply maximum batch efficiency.

For each candidate D PRE group, reason about:

D PRE duration
    ↓
UP transfer queue
    ↓
D PROC availability on affected clouds
    ↓
DOWN transfer queue
    ↓
D POST

Optimize the downstream critical path.

Do not use the previously tested naive rule:

if (idleClouds > 0)
    dispatch a small batch;

It caused severe large-workload regressions.

9. Candidate D PRE Evaluation

For each currently legal candidate group size m, estimate:

edge_finish
transfer_finish
cloud_finish[k]
downlink_finish
dpost_finish

Use only information currently known.

Do not predict future request arrivals.

It is valid to evaluate consequences of a current legal choice; it is not valid to assume future requests that have not arrived.

10. Exploit the Task-Time Table

Use the supplied piecewise-linear interpolation exactly.

For each step and batch size calculate:

amortized_cost(m)
    = (S + duration(m)) / m

and:

efficiency(m)
    = m / (S + duration(m))

Also calculate marginal cost/benefit.

But do not optimize local task efficiency in isolation.

For decode, consider the complete path:

D PRE
+ UP
+ D PROC
+ DOWN
+ D POST

11. Investigate Cloud Assignment

The remote selected by:

P PRE <remote> <rid>

is permanent.

Benchmark alternatives:

round-robin
least current workload
earliest predicted completion
least queued work

Use only current observable state.

Do not assume load balancing automatically improves score.

12. Investigate Prefill Splitting

The specification allows P PROC to be split into consecutive pieces.

Compare:

full piece
2 pieces
4 pieces
adaptive pieces

Measure impact on:

TDR

cloud utilization

total throughput

Remember every scheduled piece pays S.

Never split simply because it creates more opportunities.

13. Score-Aware Modes

The scoring parameters are available to the scheduler.

Potential regimes:

Throughput dominated

When w_tp is high, favor efficient batching when sufficient work exists.

Latency dominated

When w_c is high, reduce unnecessary waiting without blindly forcing batch size 1.

Binary waiting regime

When:

dist_base == 0

the waiting component is:

1 if dist == 0
0 if dist > 0

Treat SLO satisfaction as a constraint-like objective.

Do not assume:

S + duration(m) <= SLO2

guarantees TPOT <= SLO2. Queueing and shared resources matter.

14. Parameterized Policy, Not Hard-Coded Heuristics

Avoid rules tied to unknown future information, such as total R.

Use observable/current features:

ready_count
idle_cloud_count
busy_cloud_count
oldest_ready_age
setup cost S
batch efficiency
marginal batch efficiency
estimated downstream critical path
current queue pressure
number of remote computers represented
SLO1
SLO2
w_tp
w_c
dist_base

A candidate objective can look like:

candidate_value(m) =
    throughput_value(m)
  - cloud_idle_penalty(m)
  - edge_serialization_penalty(m)
  - waiting_penalty(m)
  - SLO_violation_penalty(m)

The coefficients must be learned/evaluated offline.

15. Search Policy Parameters Automatically

Use:

grid search
random search
coarse-to-fine search

For each parameter vector:

run training scenarios
rank candidates by mean score

Then evaluate finalists on untouched validation and holdout sets.

16. Counterexample-Driven Development

Every large loss becomes a permanent regression test.

Maintain:

tests/regressions/

Store:

seed
system parameters
scoring parameters
task-time table
arrival sequence
V2 score
candidate score
failure explanation

Every future candidate must pass the regression corpus.

17. Permanent Counterexample Classes

Always test:

Small R / many clouds

K=4 or 8
R=5..10

Large R / setup-cost dominated

K=1 or 2
R=50..100+

Sparse arrivals

Tests whether premature dispatch beats waiting.

dist_base = 0

Tests the binary waiting cliff.

w_tp = 1

Pure throughput.

w_c = 1

Pure waiting.

Mixed weights

Tests the real tradeoff.

18. Do Not Trust Tiny Perfect Benchmarks

A policy scoring 1000/1000 on a few examples proves little.

Prioritize:

mean
lower percentiles
cross-seed stability
worst regression

19. Performance Optimization Pass

Only after strategy selection.

Current benchmark:

~14.682 s / 2,000,000 frames

Target:

<=13.5 s

Investigate:

Input

unified C stdio

64KB buffering

direct pointer parsing

no stringstream

no unnecessary strings

avoid locale-sensitive parsing

State

no allocations per event

preallocated request state

compact ready queues

avoid repeated full scans

O(1) or near-O(1) transitions

Output

reusable output buffer

zero-response fast path

avoid temporary strings

minimize system calls

flush exactly once per response

Do not change protocol semantics for speed.

20. Profile Before Optimizing

Measure time spent in:

frame parsing
event parsing
state updates
strategy selection
candidate construction
output formatting
system calls

Optimize the actual hot path rather than guessing.

21. Protect Against the 15-Second Cliff

Current margin:

15.000 - 14.682 = 0.318 s

This is too small for comfort.

Run repeated benchmarks and report:

min
median
max
mean
standard deviation

Do not rely on one lucky runtime.

22. Final Validation Matrix

Before submission:

worked example
adversarial suite
event-order test
startup desync test
score benchmark
regression corpus
100k+ offline scenarios
2M internal benchmark
2M real IPC benchmark

Then rebuild from a clean tree.

23. Clean Release Build

Use the exact production build command:

clang++ -std=c++17 -O3   src/main.cpp   src/protocol.cpp   src/task_table.cpp   src/state_tracker.cpp   src/legal_tasks.cpp   src/reference_strategy.cpp   src/greedy_strategy.cpp   src/adaptive_strategy.cpp   src/conflict_resolver.cpp   src/output.cpp   -o solver

Then run the entire validation suite again.

24. Final Decision Rule

Candidate replaces V2 only if:

Candidate mean score > V2

and:

0 protocol failures
0 stuck cases
0 timeout risk
no catastrophic unexplained regressions
holdout improvement across multiple seeds
runtime safely below 15 seconds

Otherwise:

SUBMIT V2

A robust V2 is better than an unstable theoretical improvement.

25. Exact Development Order

1. Freeze safe V2
        ↓
2. Build scenario generator
        ↓
3. Validate simulator against known examples
        ↓
4. Establish V1/V2 baseline statistics
        ↓
5. Build regression corpus
        ↓
6. Analyze D PRE critical path
        ↓
7. Analyze cloud assignment
        ↓
8. Analyze prefill splitting
        ↓
9. Build parameterized candidate
        ↓
10. Search parameters offline
        ↓
11. Validate untouched validation set
        ↓
12. Validate untouched holdout set
        ↓
13. Optimize C++ hot path
        ↓
14. Re-run correctness tests
        ↓
15. Re-run 2M IPC benchmark
        ↓
16. Clean-tree rebuild
        ↓
17. Final V2 vs Candidate decision
        ↓
18. Submit

26. What NOT To Do

Do not:

blindly maximize batch size

blindly minimize batch size

use idleClouds alone

assume w_c > 0.5 means batch size 1

assume dist_base == 0 means batch size 1

use future arrivals

assume total R is known

add arbitrary batch-size limits

optimize only one task-time table

optimize only one random seed

trust a 1000/1000 microbenchmark

sacrifice correctness for speed

repeatedly edit production code without a safe checkpoint

27. Core Optimization Insight

The real question is not:

What is the best batch size?

It is:

What current legal scheduling decision minimizes the downstream critical path while balancing setup cost, cloud parallelism, transfer queues, TDR, TPOT, and the scoring weights?

Use:

current state
+
known task-time model
+
known transfer model
+
known scoring parameters

to choose the best legal action without predicting unknown future requests.

28. Winning Philosophy

The final ranking is the mean of 20 frozen final-test scores.

Therefore optimize:

expected score

not:

maximum score on a handful of scenarios

A strong policy should have:

high average
high lower-percentile score
low catastrophic-regression probability
zero protocol failures
large runtime margin

Keep:

V2 = safe fallback
V4 = experimental challenger

Only V4 replaces V2 when large unseen-style validation proves it is genuinely better.

29. Definition of Done

Safe V2 checkpoint exists.

Scenario generator covers specification dimensions.

V1/V2 baseline statistics are recorded.

100k+ scenarios evaluated.

Multiple random seeds used.

Regression corpus exists.

D PRE critical-path model investigated.

Cloud assignment investigated.

Prefill splitting investigated.

Score-aware modes tested.

Candidate parameters searched offline.

Candidate passes untouched validation.

Candidate passes untouched holdout.

No catastrophic regression remains unexplained.

Correctness suite is clean.

2M IPC test is comfortably below 15 seconds.

Runtime variance measured.

Clean-tree release build succeeds.

Final V2 vs Candidate decision documented.

Only the chosen production version is submitted.

Final Instruction to the Implementation Agent

Do not immediately modify production strategy code.

First inspect the repository and understand:

protocol
state tracker
task table interpolation
legal task generation
V1 reference
V2 greedy
V3 adaptive
conflict resolver
output path
benchmark/simulator infrastructure

Then implement the evaluation infrastructure first.

Every proposed optimization must be:

isolated,

benchmarked,

compared against V2,

tested on unseen scenarios,

checked for catastrophic regressions,

validated for protocol correctness,

measured for runtime.

Keep V2 frozen until evidence proves a replacement is safer and higher-scoring.

The goal is not to make the code more complicated. The goal is to make the final 20-test average as high and as reliable as possible while staying safely under the 15-second limit.