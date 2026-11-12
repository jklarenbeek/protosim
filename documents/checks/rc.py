import subprocess, sys
# run a command, report how it ended: exit code, or the signal that killed it
p = subprocess.run(sys.argv[2:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=float(sys.argv[1]))
out = p.stdout.decode(errors='replace')
print('returncode', p.returncode, '(killed by signal %d)' % -p.returncode if p.returncode < 0 else '', '| output %d bytes' % len(out))
for l in out.splitlines():
    if any(k in l for k in ('MAX-STEPS', 'EXIT', 'CRASH', 'exit', 'Listening', 'listening', 'GDB', 'gdb', 'Error', 'error')): print('   ', l[:110])
