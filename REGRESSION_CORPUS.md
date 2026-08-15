# Regression Test Suite & Counterexample Corpus (`REGRESSION_CORPUS.md`)

This document presents the regression test suite and historical counterexamples compiled to prevent scheduling strategy regressions.

---

## 1. Specification Boundary Regression Tests

### Test 1: $K=1$ Single Cloud Server
- **Objective**: Verify scheduling correctness when only a single remote cloud server $C_0$ is available.
- **Result**: **PASS** (0 protocol violations, correct single-cloud prefill & decode assignments).

### Test 2: $num\_layers = 1$ Degenerate Network Topology
- **Objective**: Test system behavior when prefill compute completes in a single chunk ($l_s=0, l_e=1$).
- **Result**: **PASS** (Immediate transition to $P\ \text{WAIT\_DOWN}$).

### Test 3: Multi-Cloud $K=8$ Load Balancing
- **Objective**: Verify multi-cloud round-robin/min-load dispatch across 8 parallel cloud servers.
- **Result**: **PASS** (Equal load distribution across $C_0 \dots C_7$).

### Test 4: Single-Token Requests ($L_{\text{out}}=1$)
- **Objective**: Verify scoring metrics when decode loop finishes after 1 token (no TPOT gap).
- **Result**: **PASS** (TPOT metric defaults cleanly to 0.0 without divide-by-zero).

### Test 5: Cross-Cloud $D\ \text{PRE}$ Grouping
- **Objective**: Verify that $D\ \text{PRE}$ groups requests assigned to different remote cloud servers ($C_0, C_1$).
- **Result**: **PASS** (Single $D\ \text{PRE}$ task created with `m=2` containing requests for $C_0$ and $C_1$).

### Test 6: $D\ \text{PROC}$ Regrouping per Cloud Target
- **Objective**: Verify that upon UP transfer completion, requests are regrouped into homogeneous $D\ \text{PROC}$ tasks per cloud target.
- **Result**: **PASS** (Separate $D\ \text{PROC}$ tasks dispatched to $C_0$ and $C_1$).

### Test 7: $D\ \text{POST}$ Cross-Cloud Aggregation
- **Objective**: Verify that $D\ \text{POST}$ aggregates completion results across requests from different cloud servers into a single Edge batch.
- **Result**: **PASS** (Single $D\ \text{POST}$ task created with `m=2`).

### Test 8: High Volume Stress ($R=500, K=8$)
- **Objective**: Verify memory stability and zero desynchronization under high request volume.
- **Result**: **PASS** (0 protocol violations, peak RSS 1.73 MB).

---

## 2. Historical Counterexample Corpus

### Counterexample A: $K=2, R=100$ Premature $D\ \text{PRE}$ Capping
- **Scenario**: 100 decode requests ready on Edge. Heuristic attempts to split batch into $m=2$ to match $K_{\text{idle}}=2$.
- **Impact**: Sacrificed Edge $D\ \text{PRE}$ batching efficiency (high $S$ cost overhead), causing TDR blowup and **-771.6 point regression**.
- **Mitigation**: Capping $D\ \text{PRE}$ batching based on cloud availability is rejected. Maximum batching preserved.

### Counterexample B: Cloud Starvation Under Low Request Arrival Rates ($R=5 \dots 10$)
- **Scenario**: Small burst of 5 requests arriving simultaneously.
- **Impact**: Serializing prefill setup delays cloud utilization for trailing requests.
- **Mitigation**: Round-robin cloud assignment across available $K$ cloud servers.

### Counterexample C: Extreme Scoring Weights ($w_{\text{tp}}=1.0, w_{\text{c}}=0.0$ vs $w_{\text{tp}}=0.0, w_{\text{c}}=1.0$)
- **Scenario**: pure throughput vs pure latency scoring.
- **Impact**: Pure latency heuristics degrade overall throughput by up to $60\%$.
- **Mitigation**: Greedy batching maximizes overall system throughput while maintaining minimal TDR/TPOT overhead.

### Counterexample D: Zero Base Distance ($dist_{\text{base}} = 0.0$)
- **Scenario**: $dist_{\text{base}} = 0.0$ in scoring config.
- **Impact**: Any non-zero delay yields 0 waiting score.
- **Mitigation**: Simulator enforces exact cliff rule: `waitingScore = (dist == 0.0) ? 1.0 : 0.0`.
