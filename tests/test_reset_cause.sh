#!/bin/bash
# =============================================================================
# test_reset_cause.sh — reset causes, reset vector and reset triggers
#
# Run from the repo root (needs avr-gcc for the example firmware):
#   tests/test_reset_cause.sh
# =============================================================================
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/bin/protosim"
EX="$ROOT/examples/reset_cause"
BOOT_IHEX="$ROOT/libraries/simavr/examples/board_simduino/ATmegaBOOT_168_atmega328.ihex"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0

header() { printf '\n=== %s ===\n' "$1"; }
pass()   { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail()   { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }

# run <name> <args...>: runs protosim headless; UART text in $TMP/<name>.uart, log in $TMP/<name>.log
run() {
  local name="$1"; shift
  "$SIM" "$@" --uart0-out "$TMP/$name.uart" > "$TMP/$name.log" 2>&1
  echo $? > "$TMP/$name.rc"
  sed 's/^/    /' "$TMP/$name.uart"
}
# has <name> <label> <text>: the UART output contains the line; log_has: the log does
has()     { if grep -qF -- "$3" "$TMP/$1.uart"; then pass "$2"; else fail "$2  [expected: $3]"; fi; }
log_has() { if grep -qF -- "$3" "$TMP/$1.log";  then pass "$2"; else fail "$2  [expected: $3]"; fi; }
# line <name> <n>: the n-th line of the UART output
line()    { sed -n "$2p" "$TMP/$1.uart"; }
is()      { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1  [got: $2] [expected: $3]"; fi; }

make -s -C "$EX" || { echo "cannot build the example firmware (avr-gcc?)"; exit 2; }
ELF="$EX/reset_cause.elf"

header "1. The first start is a power-on"
run t1 "$ELF" --max-steps 400000
is "MCUSR reads PORF" "$(line t1 1)" "start 01: MCUSR=01 power-on"

header "2. --reset-cause chooses what the first start reports"
run t2 "$ELF" --max-steps 400000 --reset-cause pin
is "MCUSR reads EXTRF" "$(line t2 1)" "start 01: MCUSR=02 pin"
run t2b "$ELF" --max-steps 400000 --reset-cause brownout
is "MCUSR reads BORF" "$(line t2b 1)" "start 01: MCUSR=04 brown-out"

header "3. --reset-at: one reset of every cause"
run t3 "$ELF" --max-steps 3000000 --reset-at 20 --reset-at 40:brownout --reset-at 60:watchdog --reset-at 80:power
is "start"                    "$(line t3 1)" "start 01: MCUSR=01 power-on"
is "pin is the default cause" "$(line t3 2)" "start 02: MCUSR=02 pin"
is "brown-out"                "$(line t3 3)" "start 03: MCUSR=04 brown-out"
is "watchdog"                 "$(line t3 4)" "start 04: MCUSR=08 watchdog"
is "power-on clears RAM: the start counter begins again" "$(line t3 5)" "start 01: MCUSR=01 power-on"
log_has t3 "the log names the reset" "[RST] brown-out reset at 40.000 ms"

header "4. Flags add up when firmware does not clear them"
# k: keep the flags from now on; then the reset pin, a brown-out and a power-on
run t4 "$ELF" --max-steps 4000000 --uart0-in "k" --reset-at 20:pin --reset-at 40:brownout --reset-at 60:power
is "the pin"                             "$(line t4 2)" "start 02: MCUSR=02 pin"
is "brown-out added to the pin"          "$(line t4 3)" "start 03: MCUSR=06 pin brown-out"
is "a power-on clears the other flags"   "$(line t4 4)" "start 01: MCUSR=01 power-on"

header "5. A watchdog reset made by simavr keeps the earlier flags"
# k, the reset pin at 20 ms, and w at 42 ms: the firmware arms the watchdog and hangs
run t5 "$ELF" --max-steps 4000000 --uart0-in "k........................................w" --reset-at 20:pin
is "the pin"                   "$(line t5 2)" "start 02: MCUSR=02 pin"
is "the pin and the watchdog"  "$(line t5 3)" "start 03: MCUSR=0a pin watchdog"
log_has t5 "the watchdog reset is reported" "[RST] watchdog reset at"

header "6. --uart0-in goes on after a reset"
run t6 "$ELF" --max-steps 4000000 --uart0-in "........................................x" --reset-at 20
has t6 "the character sent after the reset arrives" "[x]"

header "7. --reset-vector: a program linked at 0x7000 is started there, and again after a reset"
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -Wl,--section-start=.text=0x7000 -o "$TMP/high.elf" "$EX/reset_cause.c"
run t7 "$TMP/high.elf" --max-steps 2000000 --reset-vector 0x7000 --reset-at 20
log_has t7 "the vector is reported"      "[RST] Reset vector 0x7000"
log_has t7 "the reset leads to it"       "[RST] pin reset at 20.000 ms → PC=0x7000"
is "second start" "$(line t7 2)" "start 02: MCUSR=02 pin"

header "8. --bootloader: a reset enters the bootloader again"
if [ -f "$BOOT_IHEX" ]; then
  run t8 "$ELF" --bootloader "$BOOT_IHEX" --max-steps 300000 --reset-at 5
  log_has t8 "the reset leads to the bootloader" "[RST] pin reset at 5.000 ms → PC=0x7800"
else
  echo "  [SKIP] $BOOT_IHEX not present"
fi

header "9. SIGUSR1 pulls the reset pin of a running simulation"
"$SIM" "$ELF" --uart0-out "$TMP/t9.uart" --exit-on-uart "MCUSR=02 pin" --max-steps 2000000000 > "$TMP/t9.log" 2>&1 &
PID=$!
sleep 0.5; kill -USR1 $PID 2>/dev/null
wait $PID; RC=$?
sed 's/^/    /' "$TMP/t9.uart"
is "exit status" "$RC" "0"
has t9 "the start after the signal reports the pin" "start 02: MCUSR=02 pin"

header "10. SIGUSR2 cycles the power"
"$SIM" "$ELF" --uart0-out "$TMP/t10.uart" --reset-cause pin --exit-on-uart "MCUSR=01 power-on" --max-steps 2000000000 > "$TMP/t10.log" 2>&1 &
PID=$!
sleep 0.5; kill -USR2 $PID 2>/dev/null
wait $PID; RC=$?
is "exit status" "$RC" "0"
has t10 "the start after the signal reports a power-on" "start 01: MCUSR=01 power-on"

header "11. The PTY carries what the firmware sends, also after a reset"
"$SIM" "$ELF" > "$TMP/t11p.log" 2>&1 &
PID=$!
for i in $(seq 50); do [ -e /tmp/simavr-uart0 ] && grep -q "now points to" "$TMP/t11p.log" && break; sleep 0.1; done
stty -F /tmp/simavr-uart0 raw -echo 2>/dev/null
timeout 3 cat /tmp/simavr-uart0 > "$TMP/t11p.uart" &
sleep 0.5; kill -USR1 $PID 2>/dev/null
wait $! 2>/dev/null
kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
sed 's/^/    /' "$TMP/t11p.uart"; echo
has t11p "the start after the reset arrives on the PTY" "MCUSR=02 pin"

header "12. Malformed arguments are refused"
"$SIM" "$ELF" --reset-cause lightning > "$TMP/t12.log" 2>&1; is "--reset-cause with an unknown cause" "$?" "1"
"$SIM" "$ELF" --reset-at soon > "$TMP/t12b.log" 2>&1;        is "--reset-at without a time" "$?" "1"
"$SIM" "$ELF" --reset-at 5:lightning > "$TMP/t12c.log" 2>&1; is "--reset-at with an unknown cause" "$?" "1"
"$SIM" "$ELF" --reset-vector 0x9000 > "$TMP/t12d.log" 2>&1;  is "--reset-vector outside flash" "$?" "1"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
