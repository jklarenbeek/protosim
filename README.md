# protosim 🚀

**Cycle-accurate AVR firmware simulator — debug, profile, and cover your embedded code without hardware.**

protosim runs your compiled `.elf` or `.hex` firmware inside a bit-exact AVR emulator (simavr) and exposes a virtual UART to your host. Every instruction is observed synchronously — no sampling, no approximation. Named breakpoints, variable watches, register dumps, cycle-exact profiling, call graph, and code coverage all work from a **single command line**, zero firmware modifications required.

> **LLM quick-start:** skip to [Capability Reference](#capability-reference) — one table covers everything. Then look at [Worked Examples](#worked-examples) to see real output.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Getting Started](#getting-started)
3. [How It Works](#how-it-works)
4. [Capability Reference](#capability-reference)
5. [Worked Examples](#worked-examples)
6. [UART / Serial Output](#uart--serial-output)
7. [Troubleshooting](#troubleshooting)

---

## Prerequisites

| Platform | Toolchain |
|----------|-----------|
| **Linux / macOS** | `gcc`, `make`, `git`, `avr-gcc`, `avr-libc` |
| **Windows 11** | MSYS2 UCRT64 — see [WINDOWS.md](WINDOWS.md) |

Both platforms need `avr-gcc` to compile your own firmware; it is **not** needed to run pre-built `.elf`/`.hex` files.

---

## Getting Started

### Linux / macOS

```bash
git clone https://github.com/jklarenbeek/protosim.git
cd protosim
git submodule update --init libraries/simavr
npm install        # installs helper scripts
npm run setup      # builds libsimavr.a
npm run build      # compiles protosim → bin/protosim
```

### Windows

See **[WINDOWS.md](WINDOWS.md)** for a step-by-step guide. Short version:

```bat
git clone https://github.com/jklarenbeek/protosim.git
cd protosim
git submodule update --init libraries/simavr
scripts\build-simavr-win.bat   rem builds libsimavr.a
mingw32-make                   rem compiles protosim → bin\protosim.exe
```

### Verify the build

```
bin/protosim          # Linux — prints usage and exits
bin\protosim.exe      # Windows
```

---

## How It Works

```
your firmware (.elf / .hex)
        │
        ▼
  avr_init() + avr_load_firmware()     ← simavr creates a virtual ATmega328P
        │
        ▼
  loop: avr_run()                      ← one instruction per call
        │  after each call:
        ├─ avr->pc, avr->cycle         ← exact program counter + cycle count
        ├─ avr->flash[pre_pc]          ← opcode read for CALL/RET detection
        ├─ avr->data[0..0x8FF]         ← full register file + SRAM
        └─ breakpoint / watch / profile hooks fire here
        │
        ▼
  virtual UART0
    Linux: PTY at /tmp/simavr-uart0    ← connect picocom / minicom / screen
    Windows: TCP 127.0.0.1:4000        ← connect PuTTY in Telnet mode
```

**Why this beats sampling profilers:** every `avr_run()` call reports the exact cycle count consumed by that one instruction. There is no sample interval and no observer effect. A function called once that takes 2 instructions still appears in the profile with zero error.

**ELF vs HEX:** use `.elf` for anything involving symbol names (named breakpoints, coverage, profiling). `.hex` works for basic simulation but has no symbol table — reports show raw addresses only.

---

## Capability Reference

```
protosim <firmware.elf|.hex> [options]
```

### Core

| Flag | Argument | Effect |
|------|----------|--------|
| `-m` | `<mcu>` | MCU name (default: `atmega328p`) |
| `-f` | `<hz>` | CPU frequency (default: `16000000`) |
| `-d` | — | Start GDB server on port 1234 |
| `-v` | — | Verbose simavr peripheral log |

### Debug (ELF or HEX)

| Flag | Argument | Effect |
|------|----------|--------|
| `--max-steps` | `<n>` | Exit after N `avr_run()` calls. **Always use this** to prevent hanging. |
| `-b` | `<symbol\|0xaddr>` | Breakpoint — fires every time `avr->pc` equals the address. Supports `*` wildcard substring matching for symbols (e.g., `*do_event*`). Repeatable. |
| `-w` | `<addr:sz:name>` | Watch a named SRAM variable; printed at every breakpoint hit. `sz` = 1/2/4. |
| `--dump-regs` | — | Dump all 32 registers + SP + SREG at every breakpoint hit. |
| `--dump-sram` | `<addr:len>` | Hex+ASCII dump of a SRAM range at every breakpoint hit. |
| `-t` | `<cycles>` | Print PC + watches every N CPU cycles (time-series trace). |
| `-s` | — | Print PC after every single instruction (use with a small `--max-steps`). |

### Testing & CI (Headless Operations)

| Flag | Argument | Effect |
|------|----------|--------|
| `--uart0-in` | `<string>` | Deterministic literal string injection into UART0 RX at 9600 baud. |
| `--uart0-out` | `<file>` | Capture raw UART0 TX directly to a file (bypasses TCP/PTY). |
| `--exit-on-uart` | `<string>` | Exit simulator successfully (code 0) the moment this exact string is transmitted over UART. |
| `--wait-tcp` | — | Halt execution at cycle 0 until a TCP client connects. |

### Reset and reset cause

An AVR records why it was reset in `MCUSR`, and firmware and bootloaders branch on it: a
bootloader may wait for an upload only after a reset through the pin, an application may
want to know that the watchdog stopped it. protosim records the cause as a chip does: a
power-on sets `PORF` and clears the other flags (and RAM); any other reset adds its own
flag to those that firmware has not cleared. That also holds for a watchdog reset made by
the simulated watchdog and for `monitor reset` in GDB.

| Flag | Argument | Effect |
|------|----------|--------|
| `--reset-cause` | `<cause>` | Cause the first start reports (default: `power`). |
| `--reset-at` | `<ms>[:cause]` | Reset the part at that simulated time, counted from the start across earlier resets. Repeatable. The cause is `pin` when none is given. |
| `--reset-vector` | `<addr>` | Where a reset leads, e.g. `0x7800` for a boot section selected by `BOOTRST`. Default: address 0, or the entry of `--bootloader`. Execution also starts there. |

`<cause>` is one of `power`, `pin`, `brownout`, `watchdog`. On Linux and macOS a running
simulation also takes `kill -USR1 <pid>` as a pull on the reset pin and `kill -USR2 <pid>`
as a power cycle. Every reset is logged as `[RST] pin reset at 20.000 ms → PC=0x0000 MCUSR=0x02`.
`--uart0-in` goes on after a reset.

```bash
make -C examples/reset_cause
bin/protosim examples/reset_cause/reset_cause.elf --uart0-out out.txt --max-steps 3000000 \
    --reset-at 20 --reset-at 40:brownout --reset-at 60:power
cat out.txt
# start 01: MCUSR=01 power-on
# start 02: MCUSR=02 pin
# start 03: MCUSR=04 brown-out
# start 01: MCUSR=01 power-on        (a power-on clears RAM, so the counter starts again)
```

### Pins and analog inputs

A part without a USART talks on port pins, and what small firmware does is mostly
seen on pins. These options are the outside world for it. Times are simulated time in
ms; a pin is a port letter and a bit, `B3`.

| Flag | Argument | Effect |
|------|----------|--------|
| `--soft-uart` | `<rx>,<tx>[,<baud>[,<gap>]]` | A serial line on two pins, bridged to the PTY in place of UART0: `B0,B1,9600`. `<rx>` is the pin the part receives on. `<gap>` is the idle time between two bytes sent to the part, in bit times (default 20): firmware that takes a byte in an interrupt handler needs it. |
| `--pin-set` | `<pin>:<0\|1>[@<ms>]` | Level of an input pin, from the start or from a time. |
| `--pin-wave` | `<pin>:<period>[:<from>[:<to>]]` | Square wave into a pin, e.g. a heartbeat that stops: `B2:200:0:6000`. |
| `--pin-pull` | `<pin>:<0\|1>` | External pull on a pin: what an open-drain output reads when it is released. |
| `--pin-log` | `<pin>` | Print every change of a pin: `[PIN] B3=0 at 10016.758 ms`. |
| `--pin-duty` | `<pin>[:<window>]` | Print the share of time a pin was high, per window (default 100 ms): `[DUTY] B4 37.5% at 500.000 ms`. |
| `--adc` | `<channel\|temp>:<mV>[@<ms>]` | Voltage at an ADC input or at the temperature sensor, from the start or from a time. Repeatable. |
| `--vcc` | `<mV>[@<ms>]` | Supply voltage, as the ADC sees it. |
| `--warp` | | A sleeping part costs no real time: hours of a part that sleeps run in seconds. Also switches off simavr's short sleep on every read of a UART status register, which makes firmware that sends with polled writes run about 80 times slower than real time. |
| `--run-ms` | `<ms>` | Stop after this much simulated time. |
| `--dump-eeprom` | `<addr>:<len>` | Print that part of EEPROM when the run ends. |

When the run ends and the part has slept, the share is printed:
`[SLEEP] asleep 99.99% of 640000.000 ms, awake 42.077 ms`. `SIGTERM` ends a run like the
end of `--run-ms`, with its reports.

```bash
# an ATtiny85 that watches a board: the heartbeat on B2 stops after 6 s, the supply sags at 20 s
bin/protosim keeper.elf -m attiny85 -f 8000000 --soft-uart B0,B1,9600 --warp \
    --pin-pull B3:1 --pin-log B3 --vcc 5000 \
    --pin-wave B2:200:0:6000 --vcc 4000@20000 --vcc 5000@24000 --run-ms 30000
# [PIN] B3=0 at 10016.758 ms      the reset line, 4 s after the last heartbeat
# [PIN] B3=1 at 10112.757 ms
# [PIN] B3=0 at 20242.602 ms      held while the supply is low
# [PIN] B3=1 at 24082.594 ms
```

Two things protosim corrects in simavr here: the flags of the external and pin change
interrupts are cleared by writing a one, as on a chip (simavr sets them, or drops the
write); and the last byte of flash of an ELF file is loaded. Two things it does not:
simavr does not tell sleep modes apart, so a timer wakes a part that is powered down,
and it does not make the PWM of timer 1 of an ATtiny at its pin.

`tests/test_reset_cause.sh` checks all of this.

### Profiling (ELF only — requires symbol table)

| Flag | Argument | Effect |
|------|----------|--------|
| `--coverage` | — | Per-function code coverage % at end of run. |
| `--profile` | — | Cycle-exact flat profile sorted by self-cycles. |
| `--callgraph` | — | Call graph: per-edge call counts + cycle costs + hot path. |
| `--profile-out` | `<file>` | Write profiling reports to a file instead of stdout. |

> `--profile` and `--callgraph` always enable each other — they share the same call-stack tracking.

### Step budget guidance

| Goal | `--max-steps` |
|------|---------------|
| Does this function get called at all? | `100 000` |
| One full `loop()` iteration | `500 000` |
| Profiling with real workload | `1 000 000 – 10 000 000` |
| Startup + init only | `50 000` |

---

## Worked Examples

All output below was captured from a real `protosim.exe` run against `examples/demo/demo.elf`
(ATmega328P, 16 MHz, 5 named functions: `uart_init`, `uart_tx`, `uart_send`, `compute`, `main`
plus `unused_handler` which is dead code).

Compile the demo yourself with avr-gcc:

```bash
# Linux/macOS
cd examples/demo && make

# Windows (avr-gcc from PlatformIO or MSYS2)
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o examples/demo/demo.elf examples/demo/demo.c
avr-objcopy -O ihex -R .eeprom examples/demo/demo.elf examples/demo/demo.hex
```

---

### HEX load (no symbol table)

```
bin\protosim.exe examples\demo\demo.hex -m atmega328p -f 16000000 --max-steps 5000

Loaded HEX: 324 bytes at 0x0000
uart_com_init listening on TCP 127.0.0.1:4000
uart_com_connect: UART0 is connected to TCP port 4000

protosim running examples\demo\demo.hex on atmega328p at 16000000 Hz
[DBG] Will exit after 5000 steps.
[DBG] MAX-STEPS (5000) reached — stopping.
[DBG] Final PC=0x00b4  cycle=8365
[DBG] === Simulation Summary ===
[DBG]   Total steps : 5000
[DBG]   Total cycles: 8365
[DBG] ==============================
```

---

### Named breakpoints + variable watches

Symbol names are resolved from the ELF at startup.

```
bin\protosim.exe examples\demo\demo.elf \
  -b uart_init -b compute \
  -w 0x96:1:r24_arg \
  --max-steps 300000

[DBG] Breakpoint 'uart_init' → PC=0x0096
[DBG] Breakpoint 'compute'   → PC=0x00d4
...
[DBG] BREAKPOINT HIT: uart_init  PC=0x0096  cycle=351
[DBG] --- Watched Variables ---
  r24_arg              @ SRAM[0x0096] size=1 = 0x00 (0)
[DBG] BREAKPOINT HIT: compute  PC=0x00d4  cycle=366636
[DBG] --- Watched Variables ---
  r24_arg              @ SRAM[0x0096] size=1 = 0x00 (0)
...
[DBG] === Simulation Summary ===
[DBG]   Total steps : 300000
[DBG]   Total cycles: 499808
[DBG]   BP 'uart_init' hit 1 time(s)
[DBG]   BP 'compute' hit 8 time(s)
[DBG] ==============================
```

**Tip:** find variable SRAM addresses with:
```bash
avr-nm --demangle firmware.elf | grep -E " [dDbB] " | sort
```

---

### Register dump at breakpoint

```
bin\protosim.exe examples\demo\demo.elf -b main --dump-regs --max-steps 50000

[DBG] BREAKPOINT HIT: main  PC=0x00f6  cycle=347
[DBG] === Register Dump @ PC=0x00f6 (cycle=347) ===
  r0  = 0x00    0  .
  ...
  r24 = 0x00    0  .
  r25 = 0x00    0  .
  r26 = 0x24   36  $
  r27 = 0x01    1  .
  r28 = 0xff  255  .
  r29 = 0x08    8  .
  X=0x0124  Y=0x08ff  Z=0x0144
  SP=0x08fd
  SREG: I:0 T:0 H:0 S:0 V:0 N:0 Z:0 C:0
[DBG] =========================================
```

AVR-GCC calling convention: `r24`/`r25` = first integer argument (or return value), `r22`/`r23` = second. Break on a function and read `r24` to see what was passed in.

---

### Code coverage

```
bin\protosim.exe examples\demo\demo.elf --coverage --max-steps 1000000 \
  --profile-out coverage.txt

cat coverage.txt
```

```
[COV] ════════════════════════════════════════════════════════
[COV]  Code Coverage Report
[COV] ════════════════════════════════════════════════════════
[COV]  Function                           Words     Hit  Coverage
[COV]  --------------------------------  ------  ------  --------
[COV]  uart_init                             12       8     66.7%
[COV]  uart_tx                                7       5     71.4%
[COV]  uart_send                             12      11     91.7%
[COV]  compute                               13      13    100.0%
[COV]  unused_handler                         4       0      0.0%  ← NEVER REACHED
[COV]  main                                  19      12     63.2%
[COV]  --------------------------------  ------  ------  --------
[COV]  TOTAL                              16432      98      0.6%
[COV]
[COV]  Simulated time : 103.987 ms @ 16000000 Hz  (1663787 cycles)
[COV] ════════════════════════════════════════════════════════
```

`← NEVER REACHED` means zero flash words in that function were executed in this run. Either it is dead code, or the simulation step budget did not reach the conditions that invoke it. Increase `--max-steps` or add input via the virtual UART.

---

### Flat profile + call graph

```
bin\protosim.exe examples\demo\demo.elf \
  --profile --callgraph --max-steps 1000000 \
  --profile-out profile.txt

cat profile.txt
```

**Flat profile:**
```
[PROF] ═══════════════════════════════════════════════════════════════════
[PROF]  Flat Performance Profile  (cycle-exact — firmware unmodified)
[PROF] ═══════════════════════════════════════════════════════════════════
[PROF]  Rank  Function                Self Cycles   Self %   Calls   Cyc/Call
[PROF]  ----  ----------------------  -----------  -------  ------  ---------
[PROF]     1  uart_tx                    1645845    98.92%      92      17889  ◄ HOT
[PROF]     2  compute                      16330     0.98%      71        230
[PROF]     3  main                          1002     0.06%       1       1002
[PROF]     4  uart_send                      248     0.01%       1        248
[PROF]     5  uart_init                       15     0.00%       1         15
[PROF]
[PROF]  Total cycles   : 1663787
[PROF]  Simulated time : 103.987 ms @ 16000000 Hz
[PROF] ═══════════════════════════════════════════════════════════════════
```

**Call graph (edge summary):**
```
[CG]  Edge Summary (sorted by total cycles consumed):
[CG]  Caller           Callee           Calls  Total Cycles  % Time  Avg Cycles
[CG]  ---------------  ---------------  -----  ------------  ------  ----------
[CG]  main             uart_tx             70       1279835  76.92%       18283  ◄ HOT
[CG]  uart_send        uart_tx             21        366010  22.00%       17429  ◄ HOT
[CG]  main             uart_send            1        366258  22.01%      366258  ◄ HOT
[CG]  main             compute             71         16330   0.98%         230
[CG]  main             uart_init            1            15   0.00%          15
```

**Call tree + hot path:**
```
[CG]  Call Tree:
[CG]   main
[CG]     → uart_tx      70 calls  1279835 cyc  76.92%
[CG]     → uart_send     1 calls   366258 cyc  22.01%
[CG]       → uart_tx    21 calls   366010 cyc  22.00%
[CG]     → compute      71 calls    16330 cyc   0.98%
[CG]     → uart_init     1 calls       15 cyc   0.00%
[CG]
[CG]  Hot Path: main → uart_tx
[CG]  (hottest single edge: 100.0% of total runtime)
[CG]
[CG]  Total cycles : 1663787  (103.987 ms @ 16000000 Hz)
```

`Self cycles` = time spent inside that function's own instructions, excluding callees.
`Total cycles` in the call graph = time including everything that function called.

---

### Periodic trace

```
bin\protosim.exe examples\demo\demo.elf -t 200000 --max-steps 700000

[DBG] TRACE PC=0x00b4  cycle=200000
[DBG] TRACE PC=0x00ae  cycle=400001
[DBG] TRACE PC=0x00b2  cycle=600002
[DBG] TRACE PC=0x00b4  cycle=800002
```

Use trace to find *when* a variable changes, then switch to `-b` breakpoints to zoom in.

---

## UART / Serial Output

The simulated UART0 is bridged to your host:

| Platform | Connection |
|----------|-----------|
| Linux / macOS | PTY at `/tmp/simavr-uart0` — use `picocom -b 9600 /tmp/simavr-uart0` |
| Windows | TCP `127.0.0.1:4000` — use `putty.exe -telnet 127.0.0.1 4000` |

The bridge starts automatically when protosim launches. Your firmware's `uart_tx()` writes flow there in real time; anything you type there becomes data available to `uart_rx()`.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `Cannot create AVR core for <mcu>` | Unknown MCU name | Use `atmega328p`, `atmega2560`, etc. |
| `symbol 'X' not found — skipping` | Symbol not in ELF | Check with `avr-nm firmware.elf \| grep X` |
| Profiling reports show raw addresses | HEX input used | Recompile to ELF; HEX has no symbol table |
| Simulation never stops | No `--max-steps` | Always pass `--max-steps <n>` |
| `[DBG] CPU CRASHED` | Stack overflow or illegal opcode | Add `--dump-sram 0x0800:64` and `--dump-regs` at the last known good breakpoint |
| UART output garbled | Wrong baud | Match `BAUD` in firmware to consumer — baud is virtual, but rates must match |