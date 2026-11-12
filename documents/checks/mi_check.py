"""Drive avr-gdb in MI mode against 'protosim -d' and report what works. Evidence for the report review."""
import subprocess, sys, time, os, re, select, socket

PROTOSIM, ELF, MCU = sys.argv[1], sys.argv[2], sys.argv[3]
extra = sys.argv[4:]

def start_sim(args):
    return subprocess.Popen([PROTOSIM, ELF, '-m', MCU, '-d', '--uart0-out', '/dev/null'] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def drain(p, secs):
    out = b''; end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.05)
        if r:
            d = os.read(p.stdout.fileno(), 65536)
            if not d: break
            out += d
    return out.decode(errors='replace')

class MI:
    def __init__(self):
        self.p = subprocess.Popen(['avr-gdb', '--interpreter=mi2', '-q', '-nx', ELF], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.tok = 0; self.buf = b''; self.lines = []
        self.read_until(lambda l: l.startswith('(gdb)'), 5)
    def _pump(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if r:
            d = os.read(self.p.stdout.fileno(), 65536)
            self.buf += d
            while b'\n' in self.buf:
                l, self.buf = self.buf.split(b'\n', 1)
                self.lines.append(l.decode(errors='replace').rstrip('\r'))
            return bool(d)
        return False
    def read_until(self, pred, timeout):
        end = time.time() + timeout; seen = []
        while time.time() < end:
            while self.lines:
                l = self.lines.pop(0); seen.append(l)
                if pred(l): return l, seen
            self._pump(0.1)
        return None, seen
    def cmd(self, c, timeout=8):
        self.tok += 1; t = str(self.tok)
        self.p.stdin.write((t + c + '\n').encode()); self.p.stdin.flush()
        l, seen = self.read_until(lambda l: l.startswith(t + '^'), timeout)
        return (l or 'NO RESULT RECORD')[len(t):], seen
    def wait_stop(self, timeout=8):
        l, seen = self.read_until(lambda l: l.startswith('*stopped'), timeout)
        return l or 'NO *stopped RECORD'
def short(s, n=150): return s if len(s) <= n else s[:n] + '...'
def field(rec, name):
    m = re.search(name + r'="([^"]*)"', rec); return m.group(1) if m else None

sim = start_sim(extra)
print('protosim says:', ' | '.join(l for l in drain(sim, 0.8).splitlines() if 'GDB' in l or 'gdb' in l))
# who listens on 1234?
try:
    ss = subprocess.run(['ss', '-ltn'], capture_output=True, text=True).stdout
    print('listener:', ' / '.join(l.split()[3] for l in ss.splitlines() if ':1234' in l))
except FileNotFoundError:
    pass
# a second instance on the same fixed port
sim2 = start_sim([])
out2 = drain(sim2, 0.8)
print('second instance says:', ' | '.join(l.strip() for l in out2.splitlines() if 'GDB' in l or 'gdb' in l or 'bind' in l.lower() or 'rror' in l))
sim2.kill()

g = MI()
r, _ = g.cmd('-target-select remote localhost:1234'); print('connect:', short(r, 90))
r, _ = g.cmd('-break-insert main'); print('break main:', 'addr=' + str(field(r, 'addr')), 'line=' + str(field(r, 'line')))
r, _ = g.cmd('-exec-continue'); s = g.wait_stop(); print('continue ->', field(s, 'reason'), 'func=' + str(field(s, 'func')), 'line=' + str(field(s, 'line')))
r, _ = g.cmd('-data-evaluate-expression counter'); print('counter =', field(r, 'value'), '(0x1234 = 4660 expected after start-up)')
r, _ = g.cmd('-data-evaluate-expression "(char)banner[0]"'); print('banner[0] (a flash constant) =', field(r, 'value'), "(104 'h' expected)")
r, _ = g.cmd('-stack-list-frames'); print('frames:', short(r, 110))
r, _ = g.cmd('-exec-next'); s = g.wait_stop(); print('next ->', field(s, 'reason'), 'line=' + str(field(s, 'line')))
r, _ = g.cmd('-exec-step-instruction'); s = g.wait_stop(); print('stepi ->', field(s, 'reason'), 'addr=' + str(field(s, 'addr')))
r, _ = g.cmd('-data-list-register-values x 34'); print('PC register:', short(r, 60))
r, _ = g.cmd('-break-watch counter'); print('watch counter:', short(r, 70))
r, _ = g.cmd('-exec-continue'); s = g.wait_stop(); print('continue ->', field(s, 'reason'), short(s, 150))
flashend = 0x7fff if MCU == 'atmega328p' else 0x3ffff
for a in (flashend - 1, flashend):
    r, _ = g.cmd('-data-read-memory-bytes 0x%x 1' % a); print('read flash byte 0x%x:' % a, short(r, 100))
r, _ = g.cmd('-data-read-memory-bytes 0x8008ff 1' if MCU == 'atmega328p' else '-data-read-memory-bytes 0x8021ff 1'); print('read last RAM byte:', short(r, 80))
r, _ = g.cmd('-break-delete'); 
r, _ = g.cmd('-exec-continue'); time.sleep(0.5)
r, _ = g.cmd('-exec-interrupt', 4); s = g.wait_stop(4); print('interrupt while running ->', short(r, 40), '|', short(s, 90))
if s.startswith('NO'):
    g.p.send_signal(2); s = g.wait_stop(4); print('SIGINT to gdb ->', short(s, 90))
# the simulator while the target sits stopped in the debugger
before = drain(sim, 0.2)
time.sleep(2.0)
during = drain(sim, 0.3)
print('protosim output while the CPU is stopped for 2 s:', short(' | '.join(l.strip() for l in (before + during).splitlines() if l.strip()), 240) or '(none)')
print('protosim alive:', sim.poll() is None, 'returncode', sim.poll())
g.p.kill(); sim.kill()
