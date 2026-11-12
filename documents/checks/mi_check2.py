"""More checks against 'protosim -d': listener address, step budget while stopped, mi-async interrupt."""
import subprocess, sys, time, os, re, select, socket
sys.argv = [sys.argv[0]] + sys.argv[1:]
exec(open(os.path.join(os.path.dirname(__file__), 'mi_check.py')).read().split("sim = start_sim(extra)")[0])

# 1. is the listener reachable on a non-loopback address?
sim = start_sim([]); drain(sim, 0.6)
ip = None
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('10.255.255.255', 1)); ip = s.getsockname()[0]; s.close()
except OSError: pass
if ip and not ip.startswith('127.'):
    c = socket.socket(); c.settimeout(2)
    try: c.connect((ip, 1234)); print('1. connect to the debug port on the non-loopback address', ip, ': accepted')
    except OSError as e: print('1. connect on', ip, ':', e)
    c.close()
else:
    print('1. no non-loopback address in this environment')
sim.kill(); time.sleep(0.3)

# 2. the step budget while the CPU sits stopped in the debugger
sim = start_sim(['--max-steps', '2000000']); drain(sim, 0.6)
g = MI(); g.cmd('-target-select remote localhost:1234'); g.cmd('-break-insert main'); g.cmd('-exec-continue'); s = g.wait_stop()
print('2. stopped at', field(s, 'func'), '- now idle for 3 s with --max-steps 2000000')
time.sleep(3.0)
out = drain(sim, 0.3)
print('   protosim:', ' | '.join(l.strip() for l in out.splitlines() if 'MAX' in l or 'CPU' in l) or '(no message)', '| alive:', sim.poll() is None)
r, _ = g.cmd('-data-evaluate-expression counter', 3); print('   debugger afterwards:', short(r, 60))
g.p.kill(); sim.kill(); time.sleep(0.3)

# 3. -exec-interrupt with mi-async
sim = start_sim([]); drain(sim, 0.6)
g = MI(); r, _ = g.cmd('-gdb-set mi-async on'); g.cmd('-target-select remote localhost:1234')
r, _ = g.cmd('-exec-continue'); time.sleep(0.5)
r, _ = g.cmd('-exec-interrupt', 4); s = g.wait_stop(4); print('3. mi-async on: -exec-interrupt ->', short(r, 30), '|', short(s, 100))
g.p.kill(); sim.kill()
