# Checks behind the report

`run_checks.sh` reproduces what section 29 of
`../Protosim_Protoduino_Functional_Technical_Report.pdf` lists as executed for
Protosim itself: how a run ends, the boot HEX parser, a debug session with avr-gdb,
and the MCP server driven over stdio. `results.txt` is its recorded output at
revision `6fadc6f`.

| File | What it does |
|------|--------------|
| `run_checks.sh` | Builds the fixture, runs every check, writes `results.txt`. Modifies nothing in the repository. |
| `firmware.c` | Fixture firmware: a `.data` variable, a flash constant, a timer interrupt, one line on UART0. |
| `rc.py` | Runs a command and reports whether it exited or was killed by a signal. A shell hides the difference, and bash ends itself when its child dies of SIGINT. |
| `hex_driver.c` | Calls `protosim_read_ihex()` on the HEX fixtures the script writes. |
| `mi_check.py` | One avr-gdb session in MI mode against `protosim -d`: connect, break, step, inspect, watch, read the last flash byte, interrupt. |
| `mi_check2.py` | Listener address; `-exec-interrupt` with `mi-async on`. |
| `mi_check3.py` | The step budget while the CPU is stopped in the debugger. Takes about 20 seconds. |
| `drive.mjs` | A minimal MCP client over stdio: initialize, list tools, call them, with a timeout per call. |

Needs `bin/protosim` built, `avr-gcc`, `avr-gdb` (Ubuntu: `gdb-avr`), `node`, `python3`
and `gcc`. The MCP check uses `mcp-server/build/index.js` when it exists and the
committed bundle `mcp-server/dist/index.js` otherwise.

The checks for Protoduino (scheduler, serial path, build) and for simavr's flash and
UART model are in the Protoduino repository:
`docs/elfloader/prototype/probes/run_probes.sh` and `docs/elfloader/prototype/simprobes/`.
