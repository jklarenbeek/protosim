---
name: protosim_usage
description: Instructions for LLMs on using protosim to simulate, debug, profile, and test AVR microcontroller firmware on Windows and Linux.
---

# protosim Skill

You are an AI assistant helping a user develop, debug, profile, or test AVR firmware using `protosim`, a cycle-accurate AVR simulator (based on simavr). The `protosim` executable is assumed to be compiled and ready to use in the repository.

## 1. Operating System Context
Always determine the host OS before executing commands:
- **Linux/macOS**: Executable is `bin/protosim`. Virtual UART is exposed as a PTY at `/tmp/simavr-uart0`.
- **Windows**: Executable is `bin\protosim.exe`. Virtual UART is exposed as a TCP server at `127.0.0.1:4000`.

You can assume the protosim executable is already compiled and ready to use in the repository and in the PATH.

## 2. Firmware Compilation
While `protosim` can run `.hex` files, **you should compile and use `.elf` files** so that symbol names are available for breakpoints, profiling, and coverage.
- To compile a C file for simulation (Linux or Windows):
  ```bash
  avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o app.elf app.c
  ```
- **Note**: Always use `-g` to include debugging symbols if you plan to profile or debug.

## 3. Running protosim (CRITICAL RULES)
1. **Always use `--max-steps <n>`**. If you run `protosim` without a step limit, an infinite loop in the firmware will hang the simulation and block your execution entirely. Use step budgets intelligently:
   - Startup / init only: `--max-steps 50000`
   - One loop iteration: `--max-steps 500000`
   - Profiling with real workload: `--max-steps 1000000` to `10000000`
2. **Basic Simulation Command**:
   - Linux: `bin/protosim app.elf -m atmega328p -f 16000000 --max-steps 500000`
   - Windows: `bin\protosim.exe app.elf -m atmega328p -f 16000000 --max-steps 500000`

## 4. Key Capabilities & Flags
### Debugging Features
- `-b <symbol>`: Breakpoint on a function name (e.g., `-b main`). Fires every time the function is called.
- `-w <addr:size:name>`: Watch a variable in SRAM. Prints the value at every breakpoint.
  - *Tip for LLMs*: Find SRAM addresses automatically to set up watches: `avr-nm --demangle app.elf | grep -E " [dDbB] "`
- `--dump-regs`: Prints all registers, SP, and SREG at every breakpoint.
  - *Tip for LLMs*: Under the AVR-GCC ABI, the first integer argument to a function is passed in `r24`/`r25`, and the second in `r22`/`r23`.
- `--dump-sram <addr:len>`: Hex dump an SRAM range when hitting a breakpoint.
- `-t <cycles>`: Time-series trace; print PC and watches periodically.

### Profiling and Coverage (`.elf` required)
- `--coverage`: Print per-function code coverage percentage at the end of the simulation.
- `--profile`: Output a cycle-exact flat profile, sorted by self-cycles.
- `--callgraph`: Output edge call counts, cycle costs, and identify the hottest execution path.
- `--profile-out <file>`: Write reports to a file instead of stdout (highly recommended for LLMs to read without cluttering terminal output).

Example Profiling Commmand:
```bash
bin/protosim app.elf --profile --callgraph --coverage --max-steps 1000000 --profile-out report.txt
```

### Serial UART Interaction
If the firmware reads from or writes to `uart_rx()`/`uart_tx()`, `protosim` bridges this to the host:
- **Linux**: You can provide stimuli by writing to `/tmp/simavr-uart0` (e.g., with Python scripts, echo, or picocom).
- **Windows**: You can connect to `127.0.0.1:4000` via TCP (e.g., using a short Python socket script) to inject interactive data into the simulator while it runs.

## 5. Troubleshooting
- If step limits are reached before target behavior occurs, **increase `--max-steps`**.
- If a function shows 0% coverage (`NEVER REACHED`), ensure your step budget is high enough and that proper UART input was provided to trigger those code paths.
- MCU defaults to `atmega328p` and the clock defaults to `16000000` Hz. Specify `-m` and `-f` flags explicitly if simulating a different target.
