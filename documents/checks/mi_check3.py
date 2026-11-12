"""Does the step budget run down while the CPU is stopped in the debugger?"""
import subprocess, sys, time, os, re, select, socket
exec(open(os.path.join(os.path.dirname(__file__), 'mi_check.py')).read().split("sim = start_sim(extra)")[0])
for budget in (700,):
    sim = start_sim(['--max-steps', str(budget), '--callgraph']); drain(sim, 0.6)
    g = MI(); g.cmd('-target-select remote localhost:1234'); g.cmd('-break-insert main'); g.cmd('-exec-continue'); s = g.wait_stop()
    r, _ = g.cmd('-data-list-register-values x 34'); pc0 = field(r, 'value')
    t0 = time.time(); msg = ''
    while time.time() - t0 < 70 and sim.poll() is None:
        out = drain(sim, 0.25)
        if 'MAX-STEPS' in out: msg = [l.strip() for l in out.splitlines() if 'MAX-STEPS' in l][0]; break
    print('--max-steps %d: stopped at main (pc %s); after %.1f s without a single instruction: %s | simulator still running: %s'
          % (budget, pc0, time.time() - t0, msg or 'no message', sim.poll() is None))
    g.p.kill(); sim.kill(); time.sleep(0.3)
