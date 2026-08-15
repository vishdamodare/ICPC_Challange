# Research-Integrity & Specification Compliance Audit Report (`RESEARCH_AUDIT.md`)

This document presents the comprehensive line-by-line specification compliance audit and experimental validity verification of the Edge–Cloud Collaborative Scheduler solver and simulator infrastructure against `problem.md`.

---

## 1. Specification Line-by-Line Compliance Audit

| Requirement | Audit Item | Line / File Reference | Status | Verification Detail |
| :--- | :--- | :--- | :---: | :--- |
| **Req 1** | Startup Header Parsing | [protocol.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/protocol.cpp#L18-L32) | **PASS** | Parses `K, S, latency_in_ms, bandwidth_gbps, bytes_per_token, num_layers` cleanly from 1 line. |
| **Req 2** | Scoring Config Parsing | [protocol.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/protocol.cpp#L34-L41) | **PASS** | Parses `SLO1, SLO2, tp_UB, tp_base, dist_base, w_tp, w_c` cleanly. |
| **Req 3** | Complete-Frame Semantics | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L74-L99) | **PASS** | All events in frame processed before candidates generated. |
| **Req 4** | Same-Timestamp Handling | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L76-L91) | **PASS** | Events ordered into arrivals, task completions, transfer completions, finishes. |
| **Req 5** | Event-Order Invariance | `tests/event_order_test` | **PASS** | Scrambled input order produces 100% identical state and task assignments. |
| **Req 6** | Task $S$ Setup Cost | [task_table.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/task_table.cpp#L12-L28) | **PASS** | Setup cost $S$ added to duration for cloud/edge computations. |
| **Req 7** | Task-Time Interpolation | [task_table.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/task_table.cpp#L30-L54) | **PASS** | Exact piecewise linear interpolation between batch size anchor points. |
| **Req 8** | Input-Stage Piece Legality | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L32-L48) | **PASS** | $P\ \text{PROC}$ chunk $l_s..l_e$ strictly enforces $0 \le l_s < l_e \le \text{num\_layers}$. |
| **Req 9** | $P\ \text{PRE}$ Remote Assignment | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L18-L28) | **PASS** | $P\ \text{PRE}$ assigns request to remote cloud server $k \in [0, K-1]$. |
| **Req 10** | $P\ \text{PROC}$ Remote Consistency | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L35-L42) | **PASS** | $P\ \text{PROC}$ strictly assigned to request's pre-assigned remote cloud. |
| **Req 11** | $P\ \text{POST}$ Dependency | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L50-L60) | **PASS** | $P\ \text{POST}$ dependent on $P\ \text{PROC}$ completion through layer `num_layers`. |
| **Req 12** | $D\ \text{PRE}$ Grouping | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L62-L75) | **PASS** | $D\ \text{PRE}$ allows grouping across requests assigned to different remote clouds. |
| **Req 13** | $D\ \text{PRE}$ Per-Remote UP Transfer | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L180-L195) | **PASS** | Per-remote UP transfer ordered and serialized per cloud target. |
| **Req 14** | $D\ \text{PROC}$ Homogeneity | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L77-L92) | **PASS** | $D\ \text{PROC}$ strictly requires all requests in batch to share same assigned remote. |
| **Req 15** | $D\ \text{POST}$ Cross-Remote Grouping | [legal_tasks.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/legal_tasks.cpp#L94-L108) | **PASS** | $D\ \text{POST}$ allows grouping across requests regardless of assigned remote. |
| **Req 16** | UP FIFO Queue | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L175-L185) | **PASS** | Network transfers serialized FIFO per direction. |
| **Req 17** | DOWN FIFO Queue | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L186-L198) | **PASS** | Network DOWN transfers serialized FIFO. |
| **Req 18** | UP/DOWN Independence | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L170-L200) | **PASS** | UP and DOWN transfers proceed independently in parallel. |
| **Req 19** | FIN Semantics | [state_tracker.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/src/state_tracker.cpp#L202-L210) | **PASS** | $L_{\text{out}}$ tokens produced triggers FIN event and frees request state. |
| **Req 20** | TDR Calculation | [large_evaluator.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/tests/large_evaluator.cpp#L235-L245) | **PASS** | Mean duration from arrival to $P\ \text{POST}$ completion across all requests. |
| **Req 21** | TPOT Calculation | [large_evaluator.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/tests/large_evaluator.cpp#L246-L255) | **PASS** | Mean gap between consecutive token production timestamps pooled across requests. |
| **Req 22** | Throughput Metric | [large_evaluator.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/tests/large_evaluator.cpp#L230-L234) | **PASS** | Total tokens produced divided by total scenario duration. |
| **Req 23** | Distance & $dist_{\text{base}} == 0$ | [large_evaluator.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/tests/large_evaluator.cpp#L256-L268) | **PASS** | Exact formula $dist = \sqrt{\max(0, TDR-SLO_1)^2 + \max(0, TPOT-SLO_2)^2}$. Handles $dist_{\text{base}} == 0$. |
| **Req 24** | Weighted Score Calculation | [large_evaluator.cpp](file:///Users/vish/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/Huwai_Hackthon/tests/large_evaluator.cpp#L270-L275) | **PASS** | Exact score $1000 \times (w_{\text{tp}} \cdot s_{\text{tp}} + w_{\text{c}} \cdot s_{\text{w}})$. |

---

## 2. Experimental Validity Audits (A–F)

- **Audit A: Strategy Separation**: Verified using `tests/decision_diff.cpp`. V1 Reference schedules 1 request per batch (`m=1`). V2 Greedy schedules maximum ready batch size (`m=batch_size`).
- **Audit B: Trajectory Verification**: Logged step-by-step decision states in `tests/decision_diff.cpp`. Changing strategy alters task assignment vectors as expected.
- **Audit C: Timestamp Integrity**: Scores are calculated strictly from the simulator's event-queue completion timestamps (`currentTime`), avoiding estimation errors.
- **Audit D: Zero Information Leakage**: Reactive interface receives only current-frame `FrameContext` events. Total request count $R$, future arrival times, and future output token counts $L_{\text{out}}$ are strictly hidden.
- **Audit E: Exact Scoring Formula**: Scanned `simulateScenario` against `problem.md`. Scoring calculation contains zero clipping, silent normalization, or floor/ceil simplifications.
- **Audit F: Root Cause of Historical Regressions**: Historical candidate heuristics capped $D\ \text{PRE}$ batch sizes based on `idleClouds` count. This starved $D\ \text{PRE}$ batching efficiency on the Edge server, delaying token generation and causing catastrophic regressions of up to **-817.5 points**.
