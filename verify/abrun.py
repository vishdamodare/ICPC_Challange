import sys, random, statistics, json
sys.path.insert(0, 'verify')
from interactor import Scenario, Sim

def gen_table(rng, num_layers):
    # a handful of batch-size rows covering small and larger group sizes
    sizes = sorted(set([1, 2, 4, 8, 16, rng.choice([32,64,128,256,512,1024,2048,4096])]))
    rows = []
    for bs in sizes:
        # rough monotonic-ish costs: prefill scales with Lin-ish batch (here batch_size
        # field IS Lin for prefill rows per spec, so give it a plausible per-length cost)
        rows.append(dict(
            batch_size=bs,
            prefill_pre=round(rng.uniform(0.5, 5), 3),
            prefill_proc=round(bs * rng.uniform(0.05, 0.4) + rng.uniform(0.5, 3), 3),
            prefill_post=round(rng.uniform(0.2, 3), 3),
            decode_pre=round(rng.uniform(0.2, 3) + bs * rng.uniform(0.0, 0.05), 3),
            decode_proc=round(rng.uniform(1, 6) + bs * rng.uniform(0.0, 0.1), 3),
            decode_post=round(rng.uniform(0.2, 3) + bs * rng.uniform(0.0, 0.05), 3),
        ))
    return rows

def gen_scenario(seed):
    rng = random.Random(seed)
    K = rng.choice([1,1,2,2,4,8])
    S = rng.uniform(1, 10)
    latency = rng.uniform(0.5, 20)
    bw = rng.uniform(0.5, 20)
    bpt = rng.choice([1000, 8000, 125000, 500000])
    num_layers = rng.choice([1,2,4,8,16,32])
    table_rows = gen_table(rng, num_layers)

    R = rng.choice([1,2,4,8,16,32,64])
    arrival_mode = rng.choice(['burst', 'even', 'sparse', 'mixed'])
    arrivals = []
    t = 0.0
    for i in range(R):
        Lin = rng.randint(1, 512)
        Lout = rng.randint(1, 24)
        if arrival_mode == 'burst':
            at = 0.0 if rng.random() < 0.7 else t
        elif arrival_mode == 'even':
            at = i * rng.uniform(5, 30)
        elif arrival_mode == 'sparse':
            at = i * rng.uniform(50, 300)
        else:
            at = t + (0.0 if rng.random() < 0.4 else rng.uniform(1, 50))
        t = max(t, at)
        arrivals.append((round(at, 6), i, Lin, Lout))
    arrivals.sort(key=lambda a: (a[0], a[1]))
    # coalesce exactly-equal timestamps isn't required (spec allows simultaneous ARR events)

    w_tp = rng.choice([0.0, 0.25, 0.5, 0.75, 1.0])
    w_c = round(1.0 - w_tp, 4)
    SLO1 = rng.uniform(5, 200)
    SLO2 = rng.uniform(2, 100)
    tp_base = rng.uniform(0, 0.05)
    tp_UB = tp_base + rng.uniform(0.01, 0.5)
    dist_base = rng.choice([0.0, 0.0, rng.uniform(0.1, 3)])

    return Scenario(K=K, S=S, latency=latency, bw=bw, bpt=bpt, num_layers=num_layers,
                     table_rows=table_rows, SLO1=SLO1, SLO2=SLO2, tp_UB=tp_UB,
                     tp_base=tp_base, dist_base=dist_base, w_tp=w_tp, w_c=w_c,
                     arrivals=arrivals)


def run_batch(n, seed0, solver_cmd, tag):
    results = []
    for i in range(n):
        sc = gen_scenario(seed0 + i)
        sim = Sim(sc, solver_cmd)
        try:
            sim.run()
            score, info = sim.score()
        except Exception as e:
            score, info = 0.0, f"harness exception: {e}"
        results.append(dict(seed=seed0+i, score=score, info=(info if isinstance(info,str) else None),
                             violation=sim.violation))
    return results

if __name__ == '__main__':
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    tagname = sys.argv[3] if len(sys.argv) > 3 else 'baseline'
    cmd = ['verify/solver_baseline'] if tagname == 'baseline' else ['verify/solver_candidate']
    res = run_batch(n, seed0, cmd, tagname)
    print(json.dumps(res))