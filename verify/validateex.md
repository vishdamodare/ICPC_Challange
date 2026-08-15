import sys
sys.path.insert(0, '/home/claude/opt')
from interactor import Scenario, Sim

table_rows = [
    dict(batch_size=1, prefill_pre=3.0, prefill_proc=10.0, prefill_post=2.0,
         decode_pre=1.0, decode_proc=4.0, decode_post=1.0),
    dict(batch_size=4, prefill_pre=3.0, prefill_proc=10.0, prefill_post=2.0,
         decode_pre=1.0, decode_proc=4.0, decode_post=1.0),
]

sc = Scenario(K=1, S=1.0, latency=2.0, bw=1.0, bpt=125000, num_layers=4,
              table_rows=table_rows,
              SLO1=30.0, SLO2=15.0, tp_UB=0.0625, tp_base=0.022222222,
              dist_base=0.0, w_tp=0.5, w_c=0.5,
              arrivals=[(0.0, 0, 4, 1)])  # (time, rid, Lin, Lout)

sim = Sim(sc, ['/home/claude/opt/solver_baseline'])
sim.run()
score, info = sim.score()
print("violation:", sim.violation)
print("score:", score, info)
req = sim.reqs[0]
print("P_POST time (TDR end):", req.p_post_time, "expected 30.0")
print("token time:", req.token_times, "expected [45.0]")