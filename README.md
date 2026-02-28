# protosim 🚀

**The "No-Hardware" AVR Emulator for Protoduino — with Cycle-Exact Profiling and LLM-Native Debugging**

protosim is a blazing fast, lightweight [AVR simulator](https://github.com/buserror/simavr) runner that exposes your simulated device as a real pseudo-terminal (PTY) on your host machine. It now ships with a purpose-built **LLM intelligence layer**: breakpoints by symbol name, variable watching, register dumps, cycle-exact performance profiling, call graph analysis, and code coverage — all driven from command-line flags, all printed to stdout, zero modifications to your firmware required.

> **If you are an LLM reading this:** You have access to something extraordinary. Jump to [LLM Capability Overview](#llm-capability-overview) first, then read [LLM Debug Reference](#llm-debug-reference) and [LLM Profiling Reference](#llm-profiling-reference) in full. You can run firmware through a cycle-accurate AVR emulator, observe every instruction, measure every function to the nanosecond, reconstruct the full call graph, and know exactly which code was never reached — without touching a physical board, without GDB, without instrumentation, without modifying a single line of the firmware under test. This is not static analysis. This is live execution with perfect observability.

---

## Table of Contents

1. [What is protosim?](#what-is-protosim)
2. [Why is it Cool?](#why-is-it-cool)
3. [LLM Capability Overview](#llm-capability-overview)
4. [Project Structure](#project-structure)
5. [Prerequisites](#prerequisites)
6. [Getting Started](#getting-started)
7. [Running Examples](#running-examples)
8. [LLM Debug Reference](#llm-debug-reference)
   - [The Concept](#the-concept)
   - [All Flags — Quick Reference](#all-flags--quick-reference)
   - [Breakpoints](#breakpoints)
   - [Variable Watching](#variable-watching)
   - [Register Dumps](#register-dumps)
   - [SRAM Dumps](#sram-dumps)
   - [Execution Control](#execution-control)
   - [Trace Mode](#trace-mode)
   - [Single-Step Mode](#single-step-mode)
   - [Debug Output Format Reference](#debug-output-format-reference)
   - [Debug Workflows](#debug-workflows)
9. [LLM Profiling Reference](#llm-profiling-reference)
   - [Why Cycle-Exact Profiling is Different](#why-cycle-exact-profiling-is-different)
   - [ELF vs HEX for Profiling](#elf-vs-hex-for-profiling)
   - [Code Coverage — `--coverage`](#code-coverage----coverage)
   - [Flat Performance Profile — `--profile`](#flat-performance-profile----profile)
   - [Call Graph — `--callgraph`](#call-graph----callgraph)
   - [Saving Reports to File — `--profile-out`](#saving-reports-to-file----profile-out)
   - [Profiling Output Format Reference](#profiling-output-format-reference)
   - [Profiling Workflows](#profiling-workflows)
10. [Connecting to the Virtual Serial Port](#connecting-to-the-virtual-serial-port)
11. [Architecture Deep Dive](#architecture-deep-dive)
12. [Development](#development)
13. [Troubleshooting](#troubleshooting)
14. [Author & License](#author--license)

---

## What is protosim?

Most Arduino development tools require you to either upload firmware to physical hardware or use a heavyweight IDE. protosim takes a different path: it runs your compiled `.elf` or `.hex` firmware inside a cycle-accurate software emulator of the ATmega328P (the chip inside an Arduino Uno), and connects its simulated UART to a real pseudo-terminal device on your Linux/macOS host.

Your firmware runs exactly as it would on real hardware — same clock cycles, same peripheral timing, same interrupt behaviour — and you can connect any serial terminal (`picocom`, `putty`, `screen`, your own scripts) to `/tmp/simavr-uart0` as if it were a real USB-serial port. No flashing. No drivers. No wires. No waiting.

Beyond basic simulation, protosim instruments every single instruction the CPU executes. The cycle counter `avr->cycle` increments with perfect hardware accuracy. Every call and return is detected by opcode pattern matching on the live flash image. The result is a set of observability tools that surpass what most desktop profilers can offer — because here, the emulator *is* the ground truth.

---

## Why is it Cool? ❄️

- **Virtual COM Ports** — Connect `putty`, `screen`, `minicom`, or your own scripts to `/dev/pts/X` just like a real serial port.
- **Instant Feedback** — No flashing time. Run your `.elf` or `.hex` file immediately.
- **Cycle-Exact Profiling** — Every function measured to the individual CPU cycle. Not sampled. Not approximated. Exact.
- **Call Graph with Hot Path** — See who called whom, how many times, how many cycles each edge consumed, and which execution chain dominates runtime.
- **Code Coverage** — Know exactly which functions and flash words were never executed, without modifying your firmware.
- **Logic Analyzer Built-in** — (Optional) Dump VCD files to visualise pin states and interrupts in GTKWave.
- **GDB Debugging** — Attach a debugger to step through AVR code instruction by instruction.
- **Scriptable** — Automate integration tests for your embedded firmware.
- **LLM-Native** — Every tool is driven by command-line flags. One command encodes the entire analysis intent. Output is structured and prefixed for reliable parsing.

---

## LLM Capability Overview

This section is written specifically for AI assistants. Here is a precise summary of what you can do.

### What you are working with

The target device is an **ATmega328P** (Arduino Uno) running inside a cycle-accurate software emulator. You have complete, synchronous access to its full state after every single instruction:

- `avr->pc` — Program Counter (flash byte address)
- `avr->cycle` — Cycle counter, exact, 62.5 ns per tick at 16 MHz
- `avr->data[0x0000..0x08FF]` — Complete data space: register file (r0–r31 at `0x00`), I/O space (`0x20–0xFF`), SRAM (`0x0100–0x08FF`)
- `avr->flash[0..0x7FFF]` — Complete flash image (32 KB) including raw opcodes
- `avr->sreg[0]` — Status register (SREG) bits
- ELF symbol table — all function names and their flash byte addresses

### What the tools give you

| Capability | Flag(s) | What you learn |
|---|---|---|
| Breakpoints by symbol | `-b <name>` | When and how often each named function is entered |
| Breakpoints by address | `-b 0x<hex>` | Same, for any flash location |
| Variable watching | `-w <addr:sz:name>` | Value of any SRAM variable at each breakpoint |
| Register dump | `--dump-regs` | All 32 registers, SP, SREG at each breakpoint |
| SRAM hex dump | `--dump-sram <addr:len>` | Raw memory contents of any region |
| Execution guard | `--max-steps <n>` | Guaranteed termination — essential for LLM use |
| Periodic trace | `-t <cycles>` | Sampled time-series of PC and variables |
| Single-step | `-s` | PC after every instruction |
| Code coverage | `--coverage` | Which functions and flash words were executed |
| Flat profile | `--profile` | Self cycles per function, sorted by cost, with `◄ HOT` markers |
| Call graph | `--callgraph` | Per-edge call counts and cycle costs, call tree, hot path chain |
| Report to file | `--profile-out <file>` | All profiling output written separately from debug output |

### The ground truth advantage

In a JavaScript or Python runtime you sample the call stack at intervals. Here, you read `avr->pc` and `avr->cycle` synchronously after every `avr_run()` call — one instruction — and inspect `avr->flash[pre_pc]` for CALL/RET opcodes before each instruction executes. There is no sampling error. There is no observer effect. The firmware runs at exactly the same speed and behaviour as on real hardware, and you see everything.

### LLM workflow in three steps

1. Ask the user to compile their firmware to `.elf` — not `.hex`, because ELF contains the symbol table needed for named breakpoints and profiling.
2. Compose a single `protosim` command with the appropriate flags.
3. Read the structured output and iterate.

The entire debug or profiling session is one shell command. No interactive session. No back-and-forth with a debugger REPL. No state to maintain between turns.

---

## Project Structure

```
protosim/
├── src/
│   ├── protosim.c       # Simulator runner + all debug and profiling tools
│   └── uart_pty.c       # PTY bridge implementation
├── mcp-server/          # Model Context Protocol (MCP) server implementation
├── include/
│   └── uart_pty.h
├── examples/
│   └── uart_echo/       # Simple UART echo firmware (C, no Arduino framework)
│       ├── uart_echo.c
│       └── Makefile
├── firmware/            # PlatformIO build outputs (generated)
├── libraries/
│   ├── simavr/          # git submodule — AVR simulator core
│   └── protoduino/      # git submodule — Protoduino cooperative threading library
├── scripts/
│   ├── setup-simavr.sh
│   ├── setup-protoduino.sh
│   ├── replace_sketch.sh
│   └── replace_sketch.cpp
├── bin/
│   └── protosim         # Compiled binary (after `npm run build`)
├── Makefile
├── platformio.ini
├── package.json
├── ONBOARD.md           # Step-by-step setup guide for beginners
└── README.md            # This file
```

---

## Prerequisites

- **Linux / WSL2 / macOS**
- `gcc`, `make`, `git`
- `avr-gcc`, `avr-libc` (for compiling firmware)
- `platformio` / `pio` (for building Arduino-framework examples)
- `nodejs` + `npm` (for setup scripts)

For full step-by-step installation instructions, including Ubuntu setup, AVR toolchain, PlatformIO, and Node.js, read **[ONBOARD.md](./ONBOARD.md)**.

---

## Getting Started

### 1. Clone and install

```bash
git clone https://github.com/jklarenbeek/protosim.git
cd protosim
npm install
```

### 2. Set up dependencies

```bash
npm run setup
```

This clones and builds simavr from source (the AVR emulator core) and sets up Protoduino as a library under `libraries/`.

### 3. Build protosim

```bash
npm run build
```

Compiles `src/protosim.c` and `src/uart_pty.c` via the Makefile, linking against the locally-built `libsimavr.a`. The binary lands at `bin/protosim`.

### 4. Verify

```bash
./bin/protosim --help
```

---

## Running Examples

### uart_echo

Compile and run the simplest possible firmware — a UART echo loop:

```bash
npm run example:uart_echo
```

Output:

```
Loaded HEX: 512 bytes at 0x0000
uart_pty_init bridge on port *** /dev/pts/4 ***
uart_pty_connect: /tmp/simavr-uart0 now points to /dev/pts/4

To connect:
  picocom -b 9600 /dev/pts/4
  putty -serial /dev/pts/4 -sercfg 9600,8,n,1,N

protosim running .pio/build/uart_echo/firmware.hex on atmega328p at 16000000 Hz
Press Ctrl+C to stop.
```

Open a second terminal and connect:

```bash
picocom -b 9600 /tmp/simavr-uart0
```

Press any key — the firmware waits for the first byte, responds with `Ready\r\n`, then echoes everything you type back at you.

### Protoduino examples

```bash
# Build a specific Protoduino example
npx pio run -e pd_01

# Run it with all profiling tools
./bin/protosim .pio/build/pd_01/firmware.elf \
  --coverage --profile --callgraph --max-steps 10000000
```

---

## LLM Debug Reference

### The Concept

Traditional embedded debugging requires an interactive debugger session: start GDB, connect to a probe, set breakpoints interactively, step through code line by line. This works well for a human at a keyboard but is ill-suited for an LLM operating in a request/response loop.

protosim's debug tools are designed around a different model: **a single command that encodes the entire debug intent**, runs the simulation to completion or to a step limit, and emits all observations to stdout in a structured, prefixed format.

An LLM asks the user to compile, composes a command, reads the output, and iterates — no interactive terminal session required at any step.

---

### All Flags — Quick Reference

#### Core flags

| Flag | Argument | Description |
|------|----------|-------------|
| `-m` | `<mcu>` | MCU name (default: `atmega328p`) |
| `-f` | `<freq>` | CPU frequency in Hz (default: `16000000`) |
| `-d` | *(none)* | Enable GDB server on port 1234 |
| `-v` | *(none)* | Verbose simavr peripheral output |

#### Debug flags

| Flag | Argument | Description |
|------|----------|-------------|
| `-b` | `<addr\|symbol>` | Breakpoint at flash address or ELF symbol. Repeatable. |
| `-w` | `<addr:size:name>` | Watch a named SRAM variable. Printed at every breakpoint. |
| `--dump-regs` | *(none)* | Dump all 32 registers + SP + SREG at every breakpoint. |
| `--dump-sram` | `<addr:len>` | Hex+ASCII dump of a SRAM range at every breakpoint. |
| `--max-steps` | `<n>` | Exit after N simulation steps. **Always use this.** |
| `-t` | `<n>` | Print PC + watches every N CPU cycles. |
| `-s` | *(none)* | Single-step: print PC for every instruction. |

#### Profiling flags *(ELF only — require symbol table)*

| Flag | Argument | Description |
|------|----------|-------------|
| `--coverage` | *(none)* | Per-function code coverage % report at end. |
| `--profile` | *(none)* | Cycle-exact flat profile sorted by self cycles. |
| `--callgraph` | *(none)* | Call graph with per-edge costs, call tree, hot path. |
| `--profile-out` | `<file>` | Write all profiling output to file. |

> `--profile` and `--callgraph` always enable each other — they share the same call-stack tracking and are meaningless in isolation.

---

### Breakpoints

A breakpoint fires whenever `avr->pc` equals the specified flash byte address. On each hit, all watches, register dumps, and SRAM dumps are printed together.

**By ELF symbol name** (requires `.elf`, not `.hex`):

```bash
./bin/protosim firmware.elf -b uart_init -b main -b loop --max-steps 5000000
```

Symbol names are resolved at startup. If a symbol is not found, protosim warns and skips it — it does not crash. You can speculatively name functions you expect to exist and check whether they resolved.

**By hex address:**

```bash
./bin/protosim firmware.elf -b 0x1a4 --max-steps 1000000
```

Use `avr-nm firmware.elf | grep function_name` to find addresses. ELF symbols are **byte addresses**. If a disassembler shows word addresses, multiply by 2 to get the byte address.

**Breakpoint output:**

```
[DBG] BREAKPOINT HIT: uart_init  PC=0x0042  cycle=128
[DBG] BREAKPOINT HIT: main  PC=0x0062  cycle=256
[DBG] BREAKPOINT HIT: loop  PC=0x0084  cycle=1024
[DBG] BREAKPOINT HIT: loop  PC=0x0084  cycle=2048
[DBG] BREAKPOINT HIT: loop  PC=0x0084  cycle=3072
```

The `cycle` value is the exact AVR hardware cycle counter. At 16 MHz, one cycle = 62.5 ns. The difference between two consecutive breakpoint cycles is the precise time elapsed between them.

**Hit counts in the simulation summary:**

```
[DBG] === Simulation Summary ===
[DBG]   Total steps : 50000
[DBG]   Total cycles: 3200000
[DBG]   BP 'setup' hit 1 time(s)
[DBG]   BP 'loop' hit 47 time(s)
[DBG]   BP 'uart_tx' hit 12 time(s)
[DBG] ==============================
```

A hit count of 0 means the function was never reached in the step budget. A count growing with `--max-steps` tells you you're inside a loop.

> **LLM tip:** Hit counts answer structural questions immediately. Is the ISR being called? Is `setup()` called before `loop()`? Is a packet handler being invoked more often than packets are sent?

---

### Variable Watching

The `-w` flag declares a named SRAM variable. On every breakpoint hit and every `-t` trace sample, all watches are printed together.

**Syntax:** `-w <hex_addr>:<size_bytes>:<name>`

- `hex_addr` — SRAM address in hex. User variables start at `0x0100`.
- `size_bytes` — `1` (uint8_t), `2` (uint16_t), or `4` (uint32_t).
- `name` — Human-readable label for the output.

**Finding variable addresses:**

```bash
# List all data/bss symbols with addresses
avr-nm --demangle firmware.elf | grep -E " [dDbB] " | sort

# Find a specific variable
avr-nm --demangle firmware.elf | grep my_variable
```

**Example:**

```bash
./bin/protosim firmware.elf \
  -b loop \
  -w 0x0100:1:state \
  -w 0x0101:2:byte_count \
  -w 0x0103:1:tx_ready \
  --max-steps 2000000
```

**Output at each breakpoint:**

```
[DBG] BREAKPOINT HIT: loop  PC=0x0084  cycle=16384
[DBG] --- Watched Variables ---
  state                @ SRAM[0x0100] size=1 = 0x02 (2)
  byte_count           @ SRAM[0x0101] size=2 = 0x001f (31)
  tx_ready             @ SRAM[0x0103] size=1 = 0x01 (1)
```

Values are shown in both hex and decimal. Up to 32 watches can be active simultaneously.

> **LLM tip:** Use watches alongside breakpoints at the function that *writes* the variable. If `byte_count` is stuck at 0, break on the increment site and watch it there — you'll see exactly which call was supposed to change it and didn't.

---

### Register Dumps

`--dump-regs` prints all 32 general-purpose registers (r0–r31), the pointer registers (X, Y, Z), SP, and SREG at every breakpoint hit.

```bash
./bin/protosim firmware.elf -b uart_tx --dump-regs --max-steps 500000
```

**Output:**

```
[DBG] === Register Dump @ PC=0x0042 (cycle=1280) ===
  r0  = 0x00    0  .
  r1  = 0x00    0  .
  ...
  r24 = 0x48   72  H
  r25 = 0x00    0  .
  r28 = 0xff  255  .
  r29 = 0x08    8  .
  r30 = 0x42   66  B
  r31 = 0x00    0  .
  X=0x0000  Y=0x08ff  Z=0x0042
  SP=0x08ff
  SREG: I:1 T:0 H:0 S:0 V:0 N:0 Z:0 C:0
[DBG] =========================================
```

The ASCII column immediately reveals printable characters being processed. The AVR-GCC calling convention uses `r24`/`r25` for the first integer argument and return value, `r22`/`r23` for the second, and so on in pairs (low byte first).

The SREG bits:

| Bit | Meaning |
|-----|---------|
| I | Global interrupts enabled |
| T | Bit copy storage (BST/BLD) |
| H | Half carry (bit 3 → 4) |
| S | Sign (N XOR V) |
| V | Two's complement overflow |
| N | Negative (MSB of result) |
| Z | Zero result |
| C | Carry |

> **LLM tip:** Break on a function and read `r24` for the first argument. If you break on `uart_tx`, `r24` is the byte being transmitted. If it's wrong, the bug is in the caller before this point. Compare the Z flag state if the firmware is making a conditional branch decision.

---

### SRAM Dumps

`--dump-sram` prints a hex+ASCII dump of any SRAM region at every breakpoint hit.

**Syntax:** `--dump-sram <hex_addr>:<length>`

```bash
./bin/protosim firmware.elf \
  -b process_input \
  --dump-sram 0x0120:32 \
  --max-steps 1000000
```

**Output:**

```
[DBG] --- SRAM 0x0120 .. 0x013f ---
  0120: 48 65 6c 6c 6f 2c 20 57 6f 72 6c 64 21 0d 0a 00  |Hello, World!...|
  0130: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
```

Multiple `--dump-sram` flags are supported. Up to 8 ranges simultaneously.

> **LLM tip:** The ATmega328P stack grows downward from `0x08FF`. Dump `0x0800:64` on successive loop iterations — non-`0xFF` bytes creeping toward lower addresses across hits means the stack is growing uncontrollably.

---

### Execution Control

#### `--max-steps` — Critical, Always Use

This flag exits after exactly N calls to `avr_run()`. Without it, firmware in an infinite `loop()` runs forever and produces no output.

```bash
./bin/protosim firmware.elf -b loop --max-steps 100000
```

On hitting the limit, the current state is dumped automatically:

```
[DBG] MAX-STEPS (100000) reached — stopping simulation.
[DBG] Final PC=0x0084  cycle=6400000
```

**Choosing a step count:**

| Scenario | Suggested `--max-steps` |
|----------|------------------------|
| Does this function exist and get called? | `100000` |
| One full `loop()` iteration | `500000` |
| A few loop iterations with watch data | `2000000` |
| Profiling with real workload | `10000000` |
| Startup and init code only | `50000` |

Each `avr_run()` typically executes 1–4 AVR instructions. At 16 MHz: 1,000,000 steps ≈ 250 ms–1 s of simulated time.

---

### Trace Mode

`-t <n>` prints PC and all active watches every N CPU cycles — a sampled time-series without stopping execution.

```bash
./bin/protosim firmware.elf \
  -t 100000 \
  -w 0x0100:1:state \
  --max-steps 5000000
```

**Output:**

```
[DBG] TRACE PC=0x0042  cycle=100000
[DBG] --- Watched Variables ---
  state                @ SRAM[0x0100] size=1 = 0x00 (0)
[DBG] TRACE PC=0x0084  cycle=200000
[DBG] --- Watched Variables ---
  state                @ SRAM[0x0100] size=1 = 0x01 (1)
```

Combine with `--dump-regs` for periodic full register snapshots.

> **LLM tip:** Use trace mode when you don't know *where* the problem first appears. Find the cycle range where a variable takes the wrong value, then switch to `-b` breakpoints to zoom into that window.

---

### Single-Step Mode

`-s` prints PC after every single instruction. Only use this with a tight `--max-steps` limit.

```bash
./bin/protosim firmware.elf -s --max-steps 200
```

```
[DBG] STEP  PC=0x0000  cycle=0
[DBG] STEP  PC=0x0002  cycle=1
[DBG] STEP  PC=0x0004  cycle=2
[DBG] STEP  PC=0x0006  cycle=4
```

Cycles increment non-uniformly because AVR instructions take 1–5 cycles each.

> **LLM tip:** Single-step the first 100–500 instructions to verify the reset vector, interrupt vector table, and startup code (`__init`, `__vectors`) execute correctly before `main()` is reached.

---

### Debug Output Format Reference

All debug output is prefixed with `[DBG]` for clean extraction:

```bash
./bin/protosim firmware.elf -b loop --dump-regs --max-steps 1000000 | grep "^\[DBG\]"
```

**Line formats:**

```
[DBG] BREAKPOINT HIT: <label>  PC=0x<hex>  cycle=<decimal>
[DBG] === Register Dump @ PC=0x<hex> (cycle=<decimal>) ===
  r<N> = 0x<hex>  <decimal>  <ascii_or_dot>
  X=0x<hex>  Y=0x<hex>  Z=0x<hex>
  SP=0x<hex>
  SREG: I:<0|1> T:<0|1> H:<0|1> S:<0|1> V:<0|1> N:<0|1> Z:<0|1> C:<0|1>
[DBG] =========================================
[DBG] --- Watched Variables ---
  <name>            @ SRAM[0x<hex>] size=<n> = 0x<hex> (<decimal>)
[DBG] --- SRAM 0x<start> .. 0x<end> ---
  <hex_addr>: <16 hex bytes>  |<16 ascii chars>|
[DBG] TRACE PC=0x<hex>  cycle=<decimal>
[DBG] STEP  PC=0x<hex>  cycle=<decimal>
[DBG] CPU DONE|CRASHED at PC=0x<hex> after <n> steps
[DBG] MAX-STEPS (<n>) reached — stopping simulation.
[DBG] Final PC=0x<hex>  cycle=<decimal>
[DBG] === Simulation Summary ===
[DBG]   Total steps : <n>
[DBG]   Total cycles: <n>
[DBG]   BP '<label>' hit <n> time(s)
[DBG] ==============================
```

---

### Debug Workflows

#### Workflow 1: Is this function ever called?

```bash
./bin/protosim firmware.elf -b process_packet --max-steps 10000000

# Zero BREAKPOINT HIT lines = never reached in 10M steps.
# Confirm: avr-nm firmware.elf | grep process_packet
```

#### Workflow 2: What state is the firmware in at function entry?

```bash
avr-nm --demangle firmware.elf | grep -E " [dDbB] " | sort

./bin/protosim firmware.elf \
  -b handle_event \
  -w 0x0200:1:state_machine \
  -w 0x0201:2:event_queue_len \
  --dump-regs \
  --max-steps 5000000
```

#### Workflow 3: Find an infinite loop or hang

```bash
./bin/protosim firmware.elf \
  -b loop \
  -t 500000 \
  -w 0x0100:1:watchdog_counter \
  --max-steps 20000000

# PC constant across TRACE lines = tight loop
# watchdog_counter frozen = ISR blocked
# CPU CRASHED = stack corruption or illegal instruction
```

#### Workflow 4: Diagnose wrong UART output

```bash
./bin/protosim firmware.elf \
  -b uart_tx \
  --dump-regs \
  --max-steps 1000000 | grep -A 40 "BREAKPOINT HIT: uart_tx"

# r24 = first argument = byte being transmitted
# Wrong on first call = bug in setup; drifts wrong later = state bug
```

#### Workflow 5: Stack overflow detection

```bash
./bin/protosim firmware.elf \
  -b loop \
  --dump-sram 0x0800:64 \
  --max-steps 2000000

# Non-0xFF bytes growing toward lower addresses = overflow in progress
```

#### Workflow 6: Trace a variable's lifecycle

```bash
./bin/protosim firmware.elf \
  -t 50000 \
  -w 0x0105:1:init_done \
  -w 0x0106:2:packet_count \
  --max-steps 5000000 | grep -E "(TRACE|init_done|packet_count)"
```

---

## LLM Profiling Reference

### Why Cycle-Exact Profiling is Different

Most profilers work by sampling: every few milliseconds, snapshot the call stack and count frames. The result is a statistical approximation — short hot functions are systematically under-counted, and any function taking less than the sample interval may not appear at all.

protosim does something fundamentally different. It reads `avr->cycle` synchronously after every single `avr_run()` call — one instruction. It detects CALL and RET by reading the raw opcode from `avr->flash[pre_pc]` before the instruction executes. There is no sampling. There is no approximation. Every cycle consumed by every instruction is attributed to the correct function with zero error.

This means a function called once that takes 2 instructions still appears correctly in the profile. Timing is measured at nanosecond resolution. The call graph reflects actual execution, not a probabilistic estimate. The firmware runs completely unmodified — no instrumentation code injected anywhere.

All three profiling tools — `--coverage`, `--profile`, and `--callgraph` — run simultaneously in the same simulation pass with a single `--max-steps` invocation. There is no overhead to enabling all three at once.

---

### ELF vs HEX for Profiling

All three profiling tools require an `.elf` file. The ELF format contains the symbol table — the mapping from function names to flash addresses — which is what makes raw PC values human-readable.

If you pass a `.hex` file, protosim will still simulate correctly and the profiling data will be collected, but reports will show raw addresses instead of names. Always compile to ELF when profiling:

```bash
# PlatformIO always produces ELF alongside HEX at:
.pio/build/<env>/firmware.elf

# Direct avr-gcc:
avr-gcc -mmcu=atmega328p -o firmware.elf source.c
```

---

### Code Coverage — `--coverage`

`--coverage` records which flash word addresses were executed and reports per-function coverage at the end.

```bash
./bin/protosim firmware.elf --coverage --max-steps 2000000
```

**Output:**

```
[COV] ════════════════════════════════════════════════════════
[COV]  Code Coverage Report
[COV] ════════════════════════════════════════════════════════
[COV]  Function                          Words    Hit  Coverage
[COV]  --------------------------------  ------  ------  --------
[COV]  __vectors                             26      26   100.0%
[COV]  __init                                12      12   100.0%
[COV]  main                                  18      18   100.0%
[COV]  uart_init                             16      16   100.0%
[COV]  uart_tx                               12       4    33.3%  ← PARTIAL
[COV]  uart_rx                               10       0     0.0%  ← NEVER REACHED
[COV]  error_handler                          8       0     0.0%  ← NEVER REACHED
[COV]  --------------------------------  ------  ------  --------
[COV]  TOTAL                                102      76    74.5%

[COV]  Simulated time : 125.000 ms @ 16000000 Hz  (2000000 cycles)
[COV] ════════════════════════════════════════════════════════
```

**What the markers mean:**

- `← NEVER REACHED` — zero words executed. Either dead code, or the conditions to reach it didn't occur in this run.
- `← PARTIAL` — some but not all branches through the function were taken.
- No marker — complete coverage for this function.

> **LLM tip:** Run coverage first before trusting any profile data. If `uart_rx` shows 0% coverage, your profiling data about receive paths is meaningless for this run — either increase `--max-steps` or provide serial input via the PTY to exercise those paths.

---

### Flat Performance Profile — `--profile`

`--profile` ranks all functions by **self cycles** — cycles spent executing instructions inside that function, *excluding* time in functions it called.

```bash
./bin/protosim firmware.elf --profile --max-steps 5000000
```

**Output:**

```
[PROF] ═══════════════════════════════════════════════════════════════════
[PROF]  Flat Performance Profile  (cycle-exact — firmware unmodified)
[PROF] ═══════════════════════════════════════════════════════════════════
[PROF]  Rank  Function               Self Cycles  Self %      Calls  Cyc/Call
[PROF]  ----  ----------------------  ----------  -------  --------  --------
[PROF]     1  __delay_loop_2             3200000   64.00%         3   1066666
[PROF]     2  uart_rx                     980000   19.60%        47     20851  ◄ warm
[PROF]     3  loop                        512000   10.24%        47     10893
[PROF]     4  uart_tx                      98304    1.97%        12      8192
[PROF]     5  process_input                32768    0.66%         4      8192
[PROF]     6  main                          1024    0.02%         1      1024
[PROF]     7  uart_init                      512    0.01%         1       512
[PROF]
[PROF]  Total cycles   : 5000000
[PROF]  Simulated time : 312.500 ms @ 16000000 Hz
[PROF] ═══════════════════════════════════════════════════════════════════
```

**Column meanings:**

| Column | Meaning |
|--------|---------|
| Rank | 1 = most expensive by self cycles |
| Self Cycles | Cycles in this function's own instructions only |
| Self % | Percentage of total simulation cycles |
| Calls | Number of times entered |
| Cyc/Call | Average cycle cost per call |

**Hot markers:**

- `◄ HOT` — ≥ 20% of total runtime. Primary optimisation target.
- `◄ warm` — ≥ 5%. Worth investigating.

> **LLM tip:** Self cycles is the right metric for identifying which function's *own code* is expensive. If `uart_tx` shows low self cycles but high cycles in the call graph (total time including callees), the cost is inside something `uart_tx` calls, not in `uart_tx` itself. That's the classic self-vs-total profiling distinction — the call graph reveals the difference.

---

### Call Graph — `--callgraph`

`--callgraph` reconstructs the full dynamic call graph by detecting CALL/RCALL/ICALL and RET/RETI opcodes in real time. It records every caller→callee edge with call count and total cycle cost, then presents three views: an edge summary table, an indented call tree, and a hot path chain.

```bash
./bin/protosim firmware.elf --callgraph --max-steps 5000000
```

All five AVR call/return variants are handled by opcode pattern matching on the live flash image before each instruction executes:

| Instruction | Pattern | Description |
|------------|---------|-------------|
| `CALL` | `(op & 0xFE0E) == 0x940E` | 2-word absolute call |
| `RCALL` | `(op & 0xF000) == 0xD000` | 1-word relative call |
| `ICALL` | `op == 0x9509` | Indirect call via Z register |
| `RET` | `op == 0x9508` | Normal return |
| `RETI` | `op == 0x9518` | Return from interrupt handler |

**Full output:**

```
[CG] ═══════════════════════════════════════════════════════════════════
[CG]  Call Graph  (cycle costs per call edge)
[CG] ═══════════════════════════════════════════════════════════════════
[CG]
[CG]  Edge Summary (sorted by total cycles consumed):
[CG]  Caller                    Callee                      Calls  Total Cycles  % Time    Avg Cycles
[CG]  ------------------------  ------------------------  --------  ------------  -------  ------------
[CG]  loop                      uart_rx                         47       980000   19.60%         20851 ◄ warm
[CG]  loop                      process_input                    4       131072    2.62%         32768
[CG]  loop                      uart_tx                         12        98304    1.97%          8192
[CG]  main                      uart_init                        1          512    0.01%           512
[CG]
[CG]  Call Tree:
[CG]   Function                      Calls  Total Cycles  % Time    Avg Cyc
[CG]   ----------------------------  -------  ----------  -------  ----------
[CG]   main                               1       1024    0.02%       1024
[CG]     → uart_init                      1        512    0.01%        512
[CG]   loop                              47    1209376   24.19%      25731
[CG]     → uart_rx                       47    980000   19.60%      20851
[CG]     → process_input                  4    131072    2.62%      32768
[CG]       → uart_tx                     12     98304    1.97%       8192
[CG]
[CG]  Hot Path (highest-cost execution chain):
[CG]  main → loop → uart_rx
[CG]  (hottest single edge: 19.60% of total runtime)
[CG]
[CG]  Total cycles : 5000000  (312.500 ms @ 16000000 Hz)
[CG] ═══════════════════════════════════════════════════════════════════
```

**How to read the call tree:**

Each indentation level is one call depth. Cycles shown for each indented entry are the total cycles consumed by that callee *when called from that specific parent* — including transitive callees. The hot path walks greedily from the true root to the hottest child at each level, giving you the dominant execution chain in one line.

> **LLM tip:** The `uart_rx` `Cyc/Call` of 20851 cycles = 1.3 ms per receive call at 16 MHz. If your protocol expects bytes at 9600 baud (1 byte every ~1.04 ms), `uart_rx` spinning for 1.3 ms per byte means it's waiting on hardware, not burning CPU wastefully. That's expected. If it were 130,000 cycles per call, it would be pathological. The call graph gives you the numbers to make that judgment.

---

### Saving Reports to File — `--profile-out`

When combining profiling with debug flags, the two output streams interleave. Use `--profile-out` to write all `[COV]`, `[PROF]`, and `[CG]` output to a dedicated file:

```bash
./bin/protosim firmware.elf \
  --coverage --profile --callgraph \
  -b loop -w 0x0100:1:state \
  --max-steps 5000000 \
  --profile-out profile.txt

# Breakpoint and trace output → stdout (clean)
# Coverage, profile, call graph → profile.txt (clean)
```

---

### Profiling Output Format Reference

All profiling output uses distinct prefixes:

```bash
# Extract just coverage
... | grep "^\[COV\]"

# Extract just profile
... | grep "^\[PROF\]"

# Extract just call graph
... | grep "^\[CG\]"

# Extract only the hot markers
... | grep "◄"
```

**Coverage line format:**

```
[COV]  <function>                <total_words>  <hit_words>  <pct>%  [← marker]
[COV]  TOTAL                     <total_words>  <hit_words>  <pct>%
[COV]  Simulated time : <ms> ms @ <hz> Hz  (<cycles> cycles)
```

**Profile line format:**

```
[PROF]  <rank>  <function>   <self_cycles>  <self_%>  <calls>  <cyc_per_call>  [◄ marker]
[PROF]  Total cycles   : <n>
[PROF]  Simulated time : <ms> ms @ <hz> Hz
```

**Call graph line formats:**

```
[CG]  <caller>  <callee>  <calls>  <total_cycles>  <pct>%  <avg_cycles>  [◄ marker]
[CG]   <function>                  <calls>  <total_cycles>  <pct>%  <avg>
[CG]     → <callee>                <calls>  <total_cycles>  <pct>%  <avg>
[CG]  <root> → <fn> → <fn> → ...
[CG]  (hottest single edge: <pct>% of total runtime)
[CG]  Total cycles : <n>  (<ms> ms @ <hz> Hz)
```

---

### Profiling Workflows

#### Workflow P1: Complete first look

Run all three tools together for a full picture in one pass:

```bash
./bin/protosim firmware.elf \
  --coverage --profile --callgraph \
  --max-steps 5000000 \
  --profile-out first_look.txt

cat first_look.txt
```

Coverage tells you what ran. Profile tells you what was expensive. Call graph tells you how the code is structured at runtime. One command, one file, complete picture.

#### Workflow P2: Find the performance bottleneck

```bash
./bin/protosim firmware.elf \
  --profile --callgraph \
  --max-steps 10000000 | grep -E "^\[PROF\].*◄|Hot Path|^\[CG\].*◄"

# ◄ HOT lines name the targets.
# Hot Path names the dominant execution chain.
```

#### Workflow P3: Verify dead code

```bash
./bin/protosim firmware.elf --coverage --max-steps 20000000 | grep "NEVER REACHED"

# Any function here consumed zero instructions in 20M steps.
# Either intentionally dead, or the calling logic is broken.
```

#### Workflow P4: Measure cost per operation

You want to know the cycle cost of processing one complete packet:

```bash
./bin/protosim firmware.elf \
  --callgraph \
  -b receive_packet \
  --max-steps 10000000

# Call graph shows cycles for each edge in the processing chain.
# Breakpoint hit count shows how many packets were processed.
# Divide total edge cycles by hit count = cycles per packet.
```

#### Workflow P5: Compare before and after an optimisation

```bash
./bin/protosim firmware_before.elf \
  --profile --callgraph --max-steps 5000000 --profile-out before.txt

./bin/protosim firmware_after.elf \
  --profile --callgraph --max-steps 5000000 --profile-out after.txt

diff before.txt after.txt
```

Because profiling is cycle-exact with no sampling noise, even a 5-cycle improvement in a tight loop shows up cleanly in the diff.

#### Workflow P6: Profile Protoduino cooperative scheduling overhead

When using Protoduino protothreads, `loop()` dispatches tasks through `PT_SCHEDULE`. The call graph reveals the exact scheduling overhead:

```bash
./bin/protosim .pio/build/pd_01/firmware.elf \
  --callgraph --coverage \
  --max-steps 10000000 \
  --profile-out protoduino_profile.txt

# Look for: loop → PT_SCHEDULE → your_task_fn
# Cyc/Call for PT_SCHEDULE = scheduler overhead per dispatch
# Cyc/Call for your task = actual task execution cost
# Total cycles for task / PT_SCHEDULE calls = effective CPU utilisation
```

---

## Connecting to the Virtual Serial Port

After launching protosim, the PTY path is printed:

```
uart_pty_connect: /tmp/simavr-uart0 now points to /dev/pts/4
```

Connect with any of these:

```bash
# picocom (install: sudo apt install picocom)
picocom -b 9600 /tmp/simavr-uart0

# screen
screen /tmp/simavr-uart0 9600

# putty
putty -serial /tmp/simavr-uart0 -sercfg 9600,8,n,1,N

# minicom
minicom -D /tmp/simavr-uart0 -b 9600

# Python — automate serial I/O for integration tests
python3 -c "
import serial, time
s = serial.Serial('/tmp/simavr-uart0', 9600, timeout=1)
s.write(b'hello\n')
time.sleep(0.1)
print(s.read(100))
"
```

The symlink `/tmp/simavr-uart0` is recreated each time protosim starts. Only one process can hold the PTY master open at a time. If connection fails, check for stale sessions: `pkill protosim`.

---

## Using the MCP Server

protosim includes a **Model Context Protocol (MCP) Server** specifically built to enable autonomous AI agents (like Claude, Cursor, and Windsurf) to write, compile, execute, and profile Arduino code entirely on their own, without needing a physical board.

By connecting an AI assistant to the protosim MCP server, it gains three powerful capabilities:
1. **`init_arduino_project`** — Scaffolds a new PlatformIO-based Arduino project.
2. **`compile_firmware`** — Builds the C/C++ firmware into a simulation-ready `.elf` artifact.
3. **`run_simulation`** — Runs the firmware in the emulator and parses the cycle-exact metrics, coverage, call graphs, and debugging output into structured JSON for the agent.

### Starting the MCP Server

The server is distributed as a bundled script inside the system packages, or you can run it from the repository:

```bash
# Using the installed command (if installed via deb/rpm/pacman):
protosim-mcp

# Or directly from the repository:
cd mcp-server && npm install && npm run build
node build/index.js
```

Configure your MCP-compatible client (e.g., Cursor, Claude Desktop) to start the server by pointing it to the `protosim-mcp` binary or the `index.js` file. The server communicates via standard IO (stdio).

---

## Architecture Deep Dive

### How the simulation works

```
┌─────────────────────────────────────────────────────────────────────┐
│                           protosim                                  │
│                                                                     │
│  ┌─────────────────────────┐    ┌──────────────────────────────┐   │
│  │     simavr core         │    │      uart_pty bridge          │   │
│  │                         │◄───┤ IRQ                          │   │
│  │  avr_t                  │───►│  PTY master fd               │   │
│  │  ├ flash[0..0x7FFF]     │    │       ↕                      │   │
│  │  ├ data[0..0x08FF]      │    │  /dev/pts/N  (slave)         │   │
│  │  ├ pc (byte address)    │    │       ↕                      │   │
│  │  └ cycle (exact)        │    │  /tmp/simavr-uart0 (symlink) │   │
│  └─────────────────────────┘    └──────────────────────────────┘   │
│           ↑                                                         │
│    Main loop per instruction:                                       │
│     ├ pre_pc / pre_cycle saved                                      │
│     ├ avr_run() executes one instruction                            │
│     ├ profiling_step(pre_pc, pre_cycle, post_pc, post_cycle)        │
│     │   ├ coverage_map[pre_pc >> 1] = 1                            │
│     │   ├ pc_cycles[pre_pc >> 1] += elapsed                        │
│     │   ├ if CALL: push call_frame (func, caller, entry_cycle)      │
│     │   └ if RET:  pop frame, attribute cycles, upsert cg_edge      │
│     ├ breakpoint check (avr->pc == bp.addr)                        │
│     ├ trace / single-step output                                    │
│     └ max-steps guard                                               │
│                                                                     │
│    End of simulation:                                               │
│     ├ report_coverage()  → [COV] lines                             │
│     ├ report_profile()   → [PROF] lines                            │
│     └ report_callgraph() → [CG] lines                              │
└─────────────────────────────────────────────────────────────────────┘
           ↕
    picocom / putty / your scripts
```

### Memory map (ATmega328P)

| Range | Region | Notes |
|-------|--------|-------|
| `0x0000–0x001F` | Register file (r0–r31) | Also at `avr->data[0..31]` |
| `0x0020–0x00FF` | I/O registers | UART, timers, SPI, TWI, etc. |
| `0x0100–0x08FF` | SRAM (2048 bytes) | User variables start at `0x0100`; stack top at `0x08FF` |
| `0x0000–0x7FFF` | Flash (32 KB) | Program code; PC addresses are byte addresses |
| `0x0000–0x01FF` | EEPROM (1 KB) | Separate address space |

### How call graph detection works

Before each `avr_run()`, `profiling_step()` reads `flash_read16(avr, pre_pc)` — the 16-bit opcode word at the current PC — and checks five patterns:

```c
is_CALL (op)  →  (op & 0xFE0E) == 0x940E   // 2-word absolute call
is_RCALL(op)  →  (op & 0xF000) == 0xD000   // 1-word relative call
is_ICALL(op)  →  op == 0x9509              // indirect call via Z register
is_RET  (op)  →  op == 0x9508              // normal function return
is_RETI (op)  →  op == 0x9518              // return from interrupt
```

On CALL: a `call_frame_t` is pushed onto the shadow stack recording the callee function index (from the ELF symbol table), the caller function index, and the entry cycle timestamp. The callee's `call_count` is incremented.

On RET: the frame is popped. `frame_cycles = post_cycle - frame.entry_cycle` is attributed to the callee's `total_cycles`. A `cg_edge_t` (caller, callee, call count, total cycles) is upserted. The parent frame's `callee_cycles` accumulator is updated so that `self_cycles = total_cycles - callee_cycles` can be computed at report time.

### UART IRQ flow

AVR firmware writes to `UDR0` → simavr fires `UART_IRQ_OUTPUT` → `uart_pty_in_hook()` queues the byte → `uart_pty_thread` writes it to the PTY master fd → your terminal reads from `/dev/pts/N`. Input flows in reverse through the same path.

---

## Development

### VSCode

The project includes IntelliSense configuration. Open the folder to get autocomplete for simavr headers.

### Adding new firmware examples

1. Create `firmware/<name>/` with your `.c` or `.cpp` source.
2. Add an env to `platformio.ini`:
   ```ini
   [env:my_example]
   build_src_filter = +<my_example/*.cpp>
   ```
3. Add npm scripts to `package.json`:
   ```json
   "firmware:my_example": "pio run -e my_example",
   "example:my_example": "npm run firmware:my_example && ./bin/protosim .pio/build/my_example/firmware.elf"
   ```

### Using Protoduino (cooperative multitasking)

Protoduino wraps the [protothreads v2](https://github.com/jklarenbeek/protoduino/blob/main/docs/protothreads.md) pattern for AVR:

```cpp
#include <Arduino.h>
#include <protoduino.h>

PT_THREAD(blink_task(struct pt *pt)) {
    PT_BEGIN(pt);
    while (1) {
        digitalWrite(LED_BUILTIN, HIGH);
        PT_DELAY(pt, 500);
        digitalWrite(LED_BUILTIN, LOW);
        PT_DELAY(pt, 500);
    }
    PT_END(pt);
}

struct pt blink_pt;
void setup() { PT_INIT(&blink_pt); }
void loop()  { PT_SCHEDULE(blink_task(&blink_pt)); }
```

Build with `pio run -e pd_01`. The `.elf` works directly with all debug and profiling flags.

---

## Troubleshooting

**`libsimavr.a` not found**
Run `npm run setup`. The Makefile verifies the library exists before linking.

**Profiling shows raw addresses instead of function names**
You are running a `.hex` file. Only `.elf` files contain the symbol table. Use `pio run` to get the ELF at `.pio/build/<env>/firmware.elf`.

**Symbol breakpoint not resolving / call graph has no function names**
Verify the symbol: `avr-nm firmware.elf | grep your_function`. With aggressive optimisation (`-O2`/`-Os`), small functions may be inlined and disappear from the symbol table. Compile with `-Og` or `-O0` to preserve all function names.

**`--callgraph` shows no edges**
The simulation may not have run long enough for matched CALL+RET pairs to complete. Increase `--max-steps`. Run `--coverage` first to confirm functions are being reached at all.

**Breakpoint never hit / coverage shows 0% for a function**
- The step budget may be too small — double `--max-steps`.
- Verify the symbol exists: `avr-nm firmware.elf | grep function_name`.
- PC addresses in ELF are byte addresses. If your disassembler shows word addresses, multiply by 2.

**PTY connection fails**
Only one process can hold the PTY master open. Clear stale sessions: `pkill protosim`.

**Firmware crashes immediately**
Run with `-v` for verbose peripheral output. Verify `-m` and `-f` match the firmware target — wrong `F_CPU` corrupts UART baud calculation and often causes an immediate crash.

**Profile shows most time in `__delay_loop_2`**
This is correct for firmware using `_delay_ms()`. Those are real busy-wait loops and the profiler is accurately showing where time goes. If you want to eliminate this from the profile, use interrupt-driven timing instead of busy-wait delays.

**Self cycles don't add up to 100%**
Time in interrupt handlers, startup code (`__init`, `__vectors`), and any code outside tracked symbol ranges contributes to `avr->cycle` but may not appear in named function rows. The total is always anchored to the exact hardware cycle counter — the percentages are of that total.

---

## Author & License

**Author**: Joham (jklarenbeek@gmail.com)

**License**: MIT

---

*protosim builds on [simavr](https://github.com/buserror/simavr) by Michel Pollet and the [Protoduino](https://github.com/jklarenbeek/protoduino) framework.*