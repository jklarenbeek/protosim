#!/bin/bash
# run_checks.sh - the checks behind section 29 of the report, against this checkout.
# Needs: bin/protosim built, avr-gcc, avr-gdb, node, python3, gcc.
# Writes results.txt beside this script. Nothing in the repository is modified.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."; SIM="$ROOT/bin/protosim"
T="$(mktemp -d)"; trap 'rm -rf "$T"; pkill -x protosim 2>/dev/null' EXIT
cd "$HERE"
for m in atmega328p atmega2560; do avr-gcc -Os -g -mmcu=$m -DF_CPU=16000000UL -o "$T/fw-$m.elf" firmware.c || exit 1; done
FW="$T/fw-atmega328p.elf"
SERVER="$ROOT/mcp-server/build/index.js"; [ -f "$SERVER" ] || SERVER="$ROOT/mcp-server/dist/index.js"
{
echo "protosim $(git -C "$ROOT" rev-parse --short HEAD) | $(avr-gcc --version | head -n 1) | $(avr-gdb --version | head -n 1) | node $(node --version)"
echo; echo "== 1. how a run ends (PS01, PS11, PS12) =="
echo "step limit, default UART bridge:";  python3 rc.py 30 "$SIM" "$FW" --max-steps 2000 | head -n 2
echo "step limit, --uart0-out:";          python3 rc.py 30 "$SIM" "$FW" --max-steps 2000 --uart0-out /dev/null | head -n 2
echo "UART match:";                       python3 rc.py 30 "$SIM" "$FW" --max-steps 3000000 --exit-on-uart end | head -n 2
echo "UART match never comes:";           python3 rc.py 30 "$SIM" "$FW" --max-steps 2000 --exit-on-uart nothing | head -n 1
echo "--help:";                           python3 rc.py 30 "$SIM" --help | head -n 1; "$SIM" --help 2>&1 | grep -m1 "Cannot"
echo; echo "== 2. SRAM dump outside the modelled memory (PS05) =="
python3 rc.py 30 "$SIM" "$FW" -b main --dump-sram 0x7ff00000:16 --max-steps 20000 --uart0-out /dev/null | head -n 1
echo; echo "== 3. boot HEX parser (PS09, PS10) =="
printf ':01000000AA00\n:00000001FF\n' > "$T/bad-checksum.hex"
printf ':020000021001EB\n:01001000AA45\n:00000001FF\n' > "$T/segment.hex"
printf ':0100000055AA\n:00000001FF\n:0100100077XX\n' > "$T/after-eof.hex"
printf ':0200000011\n:00000001FF\n' > "$T/short-line.hex"
printf ':01000000ZZ00\n:00000001FF\n' > "$T/non-hex.hex"
gcc -O1 -I"$ROOT/include" -o "$T/hexdrv" hex_driver.c "$ROOT/src/ihex_loader.c" && (cd "$T" && ./hexdrv bad-checksum.hex segment.hex after-eof.hex short-line.hex non-hex.hex 2>&1)
echo; echo "== 4. a debug session: avr-gdb in MI mode against protosim -d (section 07, PS07, AV01) =="
python3 mi_check.py "$SIM" "$FW" atmega328p 2>&1 | cut -c1-200
echo "-- ATmega2560"; python3 mi_check.py "$SIM" "$T/fw-atmega2560.elf" atmega2560 2>&1 | grep -E "^connect|^break|^continue|^read flash" | cut -c1-200
echo; echo "== 5. listener address, async interrupt (PS07, section 07) =="
python3 mi_check2.py "$SIM" "$FW" atmega328p 2>&1 | grep -E "^1\.|^3\." | cut -c1-200
echo; echo "== 6. the step budget while the CPU is stopped in the debugger (PS02) =="
python3 mi_check3.py "$SIM" "$FW" atmega328p 2>&1 | cut -c1-220
echo; echo "== 7. the MCP server over stdio: $(basename "$(dirname "$SERVER")")/index.js (MC01 to MC05, MC07) =="
mkdir -p "$T/dir with space" "$T/proj/src"; cp "$FW" "$T/dir with space/fw.elf"
printf '[env:mega]\nplatform = atmelavr\nboard = megaatmega2560\nframework = arduino\n' > "$T/proj/platformio.ini"; printf '// my sketch\n' > "$T/proj/src/main.cpp"
CALLS='[
 {"label":"A. headless, ends on a UART match (protosim exits 0)","name":"run_simulation","arguments":{"elf_path":"'"$FW"'","max_steps":3000000,"breakpoints":["main"],"flags":["--coverage","--callgraph","--profile","--exit-on-uart end"]}},
 {"label":"B. step limit, default UART bridge (the documented use)","name":"run_simulation","arguments":{"elf_path":"'"$FW"'","max_steps":20000,"breakpoints":["main"]}},
 {"label":"C. firmware path with a space","name":"run_simulation","arguments":{"elf_path":"'"$T"'/dir with space/fw.elf","max_steps":1000,"flags":["--uart0-out /dev/null"]}},
 {"label":"D. init_arduino_project on an existing Mega project","name":"init_arduino_project","arguments":{"project_path":"'"$T"'/proj","env_name":"mega"}},
 {"label":"E. max_steps 0 on firmware that never ends","name":"run_simulation","arguments":{"elf_path":"'"$FW"'","max_steps":0,"flags":["--uart0-out /dev/null"]}}
]'
RPC_TIMEOUT=10 node drive.mjs "$SERVER" "$CALLS" 2>&1 | grep -E "^server|^tools|^---|breakpoints_hit|NO ANSWER|Execution completed|Cannot load|Initialized" | sed "s|$T|<tmp>|g" | cut -c1-230
echo "D left: platformio.ini = $(tr '\n' ' ' < "$T/proj/platformio.ini")| src/main.cpp starts with: $(head -n 1 "$T/proj/src/main.cpp")"
} > results.txt 2>&1
echo "wrote $HERE/results.txt"
