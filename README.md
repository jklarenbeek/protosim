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
| `-b` | `<symbol\|0xaddr>` | Breakpoint — fires every time `avr->pc` equals the address. Repeatable. |
| `-w` | `<addr:sz:name>` | Watch a named SRAM variable; printed at every breakpoint hit. `sz` = 1/2/4. |
| `--dump-regs` | — | Dump all 32 registers + SP + SREG at every breakpoint hit. |
| `--dump-sram` | `<addr:len>` | Hex+ASCII dump of a SRAM range at every breakpoint hit. |
| `-t` | `<cycles>` | Print PC + watches every N CPU cycles (time-series trace). |
| `-s` | — | Print PC after every single instruction (use with a small `--max-steps`). |

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