#!/usr/bin/env python3
"""
Faithful (but self-built, NOT the official judge) discrete-event interactor
for the Edge-Cloud Collaborative Scheduling problem, for A/B testing
candidate schedulers against the V2 baseline. Talks to a compiled C++
solver over stdin/stdout exactly per the protocol in problem.md.

Only supports solvers that always issue a single full-range P PROC piece
(ls=0, le=num_layers) -- true of both submission_baseline.cpp and our
candidate -- so piece-splitting legality isn't implemented.
"""
import subprocess, random, math, sys, heapq

class Violation(Exception):
    pass

def interpolate(rows, col, batch_size):
    pts = sorted((r['batch_size'], r[col]) for r in rows if r[col] >= 0)
    if not pts:
        return 1.0
    if batch_size <= pts[0][0]:
        return pts[0][1]
    if batch_size >= pts[-1][0]:
        return pts[-1][1]
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        if x0 <= batch_size <= x1:
            if abs(x1 - x0) < 1e-9:
                return y0
            return y0 + (batch_size - x0) * (y1 - y0) / (x1 - x0)
    return pts[-1][1]

class Scenario:
    def __init__(self, K, S, latency, bw, bpt, num_layers, table_rows,
                 SLO1, SLO2, tp_UB, tp_base, dist_base, w_tp, w_c,
                 arrivals):
        self.K = K; self.S = S; self.latency = latency; self.bw = bw
        self.bpt = bpt; self.num_layers = num_layers; self.table_rows = table_rows
        self.SLO1 = SLO1; self.SLO2 = SLO2; self.tp_UB = tp_UB
        self.tp_base = tp_base; self.dist_base = dist_base
        self.w_tp = w_tp; self.w_c = w_c
        self.arrivals = arrivals  # list of (time, rid, Lin, Lout) sorted by time

    def dur(self, tasktype, batch):
        return interpolate(self.table_rows, tasktype, batch)

    def transfer_time(self, length):
        data_bytes = length * self.bpt
        return self.latency + 8 * data_bytes / (self.bw * 1e6)


def fmt(x):
    return f"{x:.9f}"


class Request:
    __slots__ = ('rid','Lin','Lout','arrival','assigned_remote','stage',
                 'next_layer_start','decode_iter','tokens','finished',
                 'p_post_time','token_times')
    def __init__(self, rid, Lin, Lout, arrival):
        self.rid = rid; self.Lin = Lin; self.Lout = Lout; self.arrival = arrival
        self.assigned_remote = -1
        self.stage = 'ARRIVED'
        self.next_layer_start = 0
        self.decode_iter = 0
        self.tokens = 0
        self.finished = False
        self.p_post_time = None
        self.token_times = []


class Sim:
    def __init__(self, scenario: Scenario, solver_cmd):
        self.sc = scenario
        self.proc = subprocess.Popen(solver_cmd, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, text=True, bufsize=1)
        self.time = 0.0
        self.assign_counter = 0
        self.reqs = {}
        self.arrival_ptr = 0
        self.edge_busy_until = None
        self.edge_task = None
        self.cloud_busy_until = [None] * scenario.K
        self.cloud_task = [None] * scenario.K
        # transfer queues: current in-service (completion_time, kind, payload) and waiting list
        self.up_current = None   # (completion_time, remote, kind, payload)
        self.up_queue = []
        self.down_current = None
        self.down_queue = []
        self.pending_events = []  # list of dict describing events fired at self.time, filled each iter
        self.total_tokens = 0
        self.last_token_time = None
        self.total_finished = 0
        self.total_requests = len(scenario.arrivals)
        self.violation = None
        self.frames_sent = 0

    # ---------------- startup ----------------
    def send_startup(self):
        sc = self.sc
        lines = []
        lines.append(f"{sc.K} {fmt(sc.S)} {fmt(sc.latency)} {fmt(sc.bw)} {sc.bpt} {sc.num_layers}\n")
        lines.append(f"{fmt(sc.SLO1)} {fmt(sc.SLO2)} {fmt(sc.tp_UB)} {fmt(sc.tp_base)} {fmt(sc.dist_base)} {fmt(sc.w_tp)} {fmt(sc.w_c)}\n")
        lines.append(f"{len(sc.table_rows)}\n")
        for r in sc.table_rows:
            lines.append(f"{r['batch_size']} {fmt(r['prefill_pre'])} {fmt(r['prefill_proc'])} {fmt(r['prefill_post'])} {fmt(r['decode_pre'])} {fmt(r['decode_proc'])} {fmt(r['decode_post'])}\n")
        self.proc.stdin.write("".join(lines))
        self.proc.stdin.flush()

    # ---------------- helpers ----------------
    def enqueue_transfer(self, direction, remote, length, kind, payload, now):
        dur = self.sc.transfer_time(length)
        if direction == 'UP':
            if self.up_current is None:
                start = now
                comp = start + dur
                self.up_current = (comp, remote, kind, payload, length)
            else:
                self.up_queue.append((remote, kind, payload, length, dur))
        else:
            if self.down_current is None:
                start = now
                comp = start + dur
                self.down_current = (comp, remote, kind, payload, length)
            else:
                self.down_queue.append((remote, kind, payload, length, dur))

    def advance_transfer_queue(self, direction, now):
        if direction == 'UP':
            if self.up_queue:
                remote, kind, payload, length, dur = self.up_queue.pop(0)
                comp = now + dur
                self.up_current = (comp, remote, kind, payload, length)
            else:
                self.up_current = None
        else:
            if self.down_queue:
                remote, kind, payload, length, dur = self.down_queue.pop(0)
                comp = now + dur
                self.down_current = (comp, remote, kind, payload, length)
            else:
                self.down_current = None

    def next_event_time(self):
        cands = []
        if self.arrival_ptr < len(self.sc.arrivals):
            cands.append(self.sc.arrivals[self.arrival_ptr][0])
        if self.edge_busy_until is not None:
            cands.append(self.edge_busy_until)
        for t in self.cloud_busy_until:
            if t is not None:
                cands.append(t)
        if self.up_current is not None:
            cands.append(self.up_current[0])
        if self.down_current is not None:
            cands.append(self.down_current[0])
        if not cands:
            return None
        return min(cands)

    # ---------------- main loop ----------------
    def run(self, max_frames=2_000_000):
        self.send_startup()
        while True:
            if self.total_finished >= self.total_requests and self.total_requests > 0:
                self.proc.stdin.write("END\n")
                try:
                    self.proc.stdin.flush()
                except Exception:
                    pass
                break
            nt = self.next_event_time()
            if nt is None:
                self.violation = "stuck: no future event"
                break
            self.time = nt
            events = self.collect_events(nt)
            self.send_frame(events)
            ok = self.read_and_apply_response()
            if not ok:
                break
            self.frames_sent += 1
            if self.frames_sent > max_frames:
                self.violation = "frame limit exceeded"
                break
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()

    def collect_events(self, t):
        evs = []
        # ARR events
        while self.arrival_ptr < len(self.sc.arrivals) and self.sc.arrivals[self.arrival_ptr][0] == t:
            _, rid, Lin, Lout = self.sc.arrivals[self.arrival_ptr]
            self.reqs[rid] = Request(rid, Lin, Lout, t)
            evs.append(('ARR', rid, Lin))
            self.arrival_ptr += 1

        # TDN events (edge + cloud), ordered by assignment counter for determinism
        tdn_list = []
        if self.edge_busy_until == t:
            tdn_list.append(('E', self.edge_task))
        for k in range(self.sc.K):
            if self.cloud_busy_until[k] == t:
                tdn_list.append((f'C{k}', self.cloud_task[k]))
        tdn_list.sort(key=lambda x: x[1]['assign_id'])

        fins = []
        for server, task in tdn_list:
            evs.append(('TDN', server, task))
            self.on_task_complete(server, task, t, fins)
            if server == 'E':
                self.edge_busy_until = None
                self.edge_task = None
            else:
                k = int(server[1:])
                self.cloud_busy_until[k] = None
                self.cloud_task[k] = None

        # XDN events
        if self.up_current is not None and self.up_current[0] == t:
            comp, remote, kind, payload, length = self.up_current
            evs.append(('XDN', 'UP', remote, length, kind, payload))
            self.on_transfer_complete('UP', remote, kind, payload, t)
            self.advance_transfer_queue('UP', t)
        if self.down_current is not None and self.down_current[0] == t:
            comp, remote, kind, payload, length = self.down_current
            evs.append(('XDN', 'DOWN', remote, length, kind, payload))
            self.on_transfer_complete('DOWN', remote, kind, payload, t)
            self.advance_transfer_queue('DOWN', t)

        for rid in fins:
            evs.append(('FIN', rid))
        return evs

    def on_task_complete(self, server, task, t, fins):
        ttype = task['type']
        if ttype == 'P_PRE':
            rid = task['requests'][0]
            req = self.reqs[rid]
            req.assigned_remote = task['remote']
            self.enqueue_transfer('UP', task['remote'], req.Lin, 'PRE', rid, t)
        elif ttype == 'P_PROC':
            rid = task['requests'][0]
            req = self.reqs[rid]
            req.next_layer_start = self.sc.num_layers
            self.enqueue_transfer('DOWN', task['remote'], req.Lin, 'PRE', rid, t)
        elif ttype == 'P_POST':
            rid = task['requests'][0]
            req = self.reqs[rid]
            req.p_post_time = t
            req.stage = 'D_PRE_READY'
        elif ttype == 'D_PRE':
            # group by remote for per-remote UP transfers, increasing remote order
            by_remote = {}
            for rid in task['requests']:
                r = self.reqs[rid].assigned_remote
                by_remote.setdefault(r, []).append(rid)
            for r in sorted(by_remote.keys()):
                members = by_remote[r]
                self.enqueue_transfer('UP', r, len(members), 'DEC', tuple(sorted(members)), t)
        elif ttype == 'D_PROC':
            remote = task['remote']
            members = tuple(sorted(task['requests']))
            self.enqueue_transfer('DOWN', remote, len(members), 'DEC', members, t)
        elif ttype == 'D_POST':
            for rid in task['requests']:
                req = self.reqs[rid]
                req.tokens += 1
                req.decode_iter += 1
                req.token_times.append(t)
                self.total_tokens += 1
                if self.last_token_time is None or t > self.last_token_time:
                    self.last_token_time = t
                if req.decode_iter >= req.Lout:
                    req.finished = True
                    req.stage = 'FINISHED'
                    fins.append(rid)
                    self.total_finished += 1
                else:
                    req.stage = 'D_PRE_READY'

    def on_transfer_complete(self, direction, remote, kind, payload, t):
        if kind == 'PRE':
            rid = payload
            req = self.reqs[rid]
            if direction == 'UP':
                req.stage = 'P_PROC_READY'
            else:
                req.stage = 'P_POST_READY'
        else:  # DEC
            members = payload
            if direction == 'UP':
                for rid in members:
                    self.reqs[rid].stage = 'D_PROC_READY'
            else:
                for rid in members:
                    self.reqs[rid].stage = 'D_POST_READY'

    # ---------------- I/O with solver ----------------
    def send_frame(self, events):
        lines = [fmt(self.time) + "\n", f"{len(events)}\n"]
        for ev in events:
            if ev[0] == 'ARR':
                lines.append(f"ARR {ev[1]} {ev[2]}\n")
            elif ev[0] == 'TDN':
                server, task = ev[1], ev[2]
                lines.append(f"TDN {server} {task['echo']} {fmt(task['dur'])}\n")
            elif ev[0] == 'XDN':
                _, direction, remote, length, kind, payload = ev
                size = length * self.sc.bpt
                if kind == 'PRE':
                    rids = [payload]
                else:
                    rids = list(payload)
                lines.append(f"XDN {direction} {remote} {size} {kind} {len(rids)} " + " ".join(map(str, rids)) + "\n")
            elif ev[0] == 'FIN':
                lines.append(f"FIN {ev[1]}\n")
        self.proc.stdin.write("".join(lines))
        self.proc.stdin.flush()

    def read_and_apply_response(self):
        line = self.proc.stdout.readline()
        if line == '':
            self.violation = "solver closed stdout / EOF"
            return False
        try:
            n = int(line.strip())
        except Exception:
            self.violation = f"malformed count line: {line!r}"
            return False
        assigns = []
        for _ in range(n):
            l = self.proc.stdout.readline()
            if l == '':
                self.violation = "EOF mid-response"
                return False
            assigns.append(l.split())
        try:
            self.apply_assignments(assigns)
        except Violation as v:
            self.violation = str(v)
            return False
        return True

    def apply_assignments(self, assigns):
        edge_used = self.edge_busy_until is not None
        cloud_used = [self.cloud_busy_until[k] is not None for k in range(self.sc.K)]
        for toks in assigns:
            if not toks:
                raise Violation("empty assignment line")
            server = toks[0]
            if server == 'E':
                if edge_used:
                    raise Violation("edge double-assigned")
                self.apply_edge(toks)
                edge_used = True
            elif server.startswith('C'):
                k = int(server[1:])
                if k < 0 or k >= self.sc.K:
                    raise Violation("bad remote id")
                if cloud_used[k]:
                    raise Violation(f"cloud {k} double-assigned")
                self.apply_cloud(k, toks)
                cloud_used[k] = True
            else:
                raise Violation("bad server token")

    def req_ok_predecessor(self, rid, expected_stage):
        req = self.reqs.get(rid)
        if req is None or req.finished:
            raise Violation(f"request {rid} unknown/finished")
        if req.stage != expected_stage:
            raise Violation(f"request {rid} not ready for {expected_stage} (at {req.stage})")

    def apply_edge(self, toks):
        kind = toks[1]
        t = self.time
        if kind == 'P' and toks[2] == 'PRE':
            remote = int(toks[3]); rid = int(toks[4])
            self.req_ok_predecessor(rid, 'ARRIVED')
            if remote < 0 or remote >= self.sc.K:
                raise Violation("P PRE remote out of range")
            req = self.reqs[rid]
            req.stage = 'P_PRE_INFLIGHT'
            dur = self.sc.dur('prefill_pre', req.Lin)
            echo = f"P PRE {remote} {rid}"
            self.edge_busy_until = t + self.sc.S + dur
            self.edge_task = dict(type='P_PRE', requests=[rid], remote=remote, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        elif kind == 'P' and toks[2] == 'POST':
            remote = int(toks[3]); rid = int(toks[4])
            self.req_ok_predecessor(rid, 'P_POST_READY')
            req = self.reqs[rid]
            if remote != req.assigned_remote:
                raise Violation("P POST wrong remote")
            req.stage = 'P_POST_INFLIGHT'
            dur = self.sc.dur('prefill_post', req.Lin)
            echo = f"P POST {remote} {rid}"
            self.edge_busy_until = t + self.sc.S + dur
            self.edge_task = dict(type='P_POST', requests=[rid], remote=remote, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        elif kind == 'D' and toks[2] == 'PRE':
            m = int(toks[4])
            rids = list(map(int, toks[5:5+m]))
            if len(set(rids)) != m or m < 1:
                raise Violation("bad D PRE group")
            for rid in rids:
                self.req_ok_predecessor(rid, 'D_PRE_READY')
            for rid in rids:
                self.reqs[rid].stage = 'D_PRE_INFLIGHT'
            dur = self.sc.dur('decode_pre', m)
            echo = f"D PRE -1 {m} " + " ".join(map(str, rids))
            self.edge_busy_until = self.time + self.sc.S + dur
            self.edge_task = dict(type='D_PRE', requests=rids, remote=-1, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        elif kind == 'D' and toks[2] == 'POST':
            m = int(toks[4])
            rids = list(map(int, toks[5:5+m]))
            if len(set(rids)) != m or m < 1:
                raise Violation("bad D POST group")
            for rid in rids:
                self.req_ok_predecessor(rid, 'D_POST_READY')
            for rid in rids:
                self.reqs[rid].stage = 'D_POST_INFLIGHT'
            dur = self.sc.dur('decode_post', m)
            echo = f"D POST -1 {m} " + " ".join(map(str, rids))
            self.edge_busy_until = self.time + self.sc.S + dur
            self.edge_task = dict(type='D_POST', requests=rids, remote=-1, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        else:
            raise Violation("unrecognized edge task")

    def apply_cloud(self, k, toks):
        kind = toks[1]
        t = self.time
        if kind == 'P' and toks[2] == 'PROC':
            ls = int(toks[3]); le = int(toks[4]); remote = int(toks[5]); rid = int(toks[6])
            if remote != k:
                raise Violation("P PROC server/remote mismatch")
            req = self.reqs.get(rid)
            if req is None or req.finished:
                raise Violation("bad rid for P PROC")
            if req.assigned_remote != k:
                raise Violation("P PROC wrong remote for request")
            if req.stage != 'P_PROC_READY':
                raise Violation(f"P PROC not ready ({req.stage})")
            if ls != req.next_layer_start or le != self.sc.num_layers or ls >= le:
                raise Violation("illegal P PROC piece (only full pieces supported by this harness)")
            req.stage = 'P_PROC_INFLIGHT'
            frac = (le - ls) / self.sc.num_layers
            dur = frac * self.sc.dur('prefill_proc', req.Lin)
            echo = f"P PROC {ls} {le} {remote} {rid}"
            self.cloud_busy_until[k] = t + self.sc.S + dur
            self.cloud_task[k] = dict(type='P_PROC', requests=[rid], remote=k, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        elif kind == 'D' and toks[2] == 'PROC':
            remote = int(toks[3]); m = int(toks[4])
            rids = list(map(int, toks[5:5+m]))
            if remote != k:
                raise Violation("D PROC server/remote mismatch")
            if len(set(rids)) != m or m < 1:
                raise Violation("bad D PROC group")
            for rid in rids:
                req = self.reqs.get(rid)
                if req is None or req.finished:
                    raise Violation("bad rid for D PROC")
                if req.assigned_remote != k:
                    raise Violation("D PROC member wrong remote")
                if req.stage != 'D_PROC_READY':
                    raise Violation(f"D PROC not ready ({req.stage})")
            for rid in rids:
                self.reqs[rid].stage = 'D_PROC_INFLIGHT'
            dur = self.sc.dur('decode_proc', m)
            echo = f"D PROC {remote} {m} " + " ".join(map(str, rids))
            self.cloud_busy_until[k] = t + self.sc.S + dur
            self.cloud_task[k] = dict(type='D_PROC', requests=rids, remote=k, dur=dur, echo=echo, assign_id=self.assign_counter)
            self.assign_counter += 1
        else:
            raise Violation("unrecognized cloud task")

    # ---------------- scoring ----------------
    def score(self):
        if self.violation:
            return 0.0, self.violation
        if self.total_finished < self.total_requests:
            return 0.0, "not all requests finished"
        first_arrival = self.sc.arrivals[0][0]
        elapsed = self.last_token_time - first_arrival
        tp = self.total_tokens / elapsed if elapsed > 0 else 0.0
        tp_comp = max(0.0, min(1.0, (tp - self.sc.tp_base) / (self.sc.tp_UB - self.sc.tp_base)))

        tdrs = []
        gaps = []
        for rid, req in self.reqs.items():
            tdrs.append(req.p_post_time - req.arrival)
            tt = req.token_times
            for i in range(1, len(tt)):
                gaps.append(tt[i] - tt[i-1])
        tdr = sum(tdrs) / len(tdrs)
        tpot = sum(gaps) / len(gaps) if gaps else 0.0

        excess_tdr = max(0.0, (tdr - self.sc.SLO1) / self.sc.SLO1)
        excess_tpot = max(0.0, (tpot - self.sc.SLO2) / self.sc.SLO2)
        dist = math.sqrt(excess_tdr**2 + excess_tpot**2)

        if self.sc.dist_base > 0:
            wc_comp = max(0.0, 1.0 - dist / self.sc.dist_base)
        else:
            wc_comp = 1.0 if dist == 0 else 0.0

        norm = self.sc.w_tp * tp_comp + self.sc.w_c * wc_comp
        return 1000.0 * norm, dict(tp=tp, tdr=tdr, tpot=tpot, dist=dist,
                                    tp_comp=tp_comp, wc_comp=wc_comp)


def run_one(scenario, solver_cmd):
    sim = Sim(scenario, solver_cmd)
    sim.run()
    return sim.score()