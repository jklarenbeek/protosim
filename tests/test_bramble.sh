#!/bin/bash
# =============================================================================
# test_bramble.sh — the RP2040 and RP2350 target (Bramble) through protosim
#
# Run from the repo root, after `make` with the Bramble submodule checked out:
#   PICO_SDK_PATH=<pico-sdk> tests/test_bramble.sh
# Without PICO_SDK_PATH the examples must be built already (examples/rp2/out).
# Needs cmake and arm-none-eabi-gcc to build them.
# =============================================================================
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/bin/protosim"
EX="$ROOT/examples/rp2"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; [ -n "$SIMPID" ] && kill $SIMPID 2>/dev/null' EXIT
PASS=0; FAIL=0

header() { printf '\n=== %s ===\n' "$1"; }
pass()   { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail()   { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }

# run <name> <args...>: headless; UART text in $TMP/<name>.uart, log in $TMP/<name>.log
run() {
  local name="$1"; shift
  "$SIM" "$@" --uart0-out "$TMP/$name.uart" > "$TMP/$name.log" 2>&1
  echo $? > "$TMP/$name.rc"
}
has()     { if grep -qF -- "$3" "$TMP/$1.uart"; then pass "$2"; else fail "$2  [expected: $3]"; sed 's/^/      /' "$TMP/$1.uart" | head -8; fi; }
log_has() { if grep -qF -- "$3" "$TMP/$1.log";  then pass "$2"; else fail "$2  [expected: $3]"; grep -v '^\[\(Init\|ELF\|Boot\)' "$TMP/$1.log" | tail -5 | sed 's/^/      /'; fi; }
count()   { grep -cF -- "$2" "$TMP/$1.${3:-log}"; }

"$SIM" 2>&1 | grep -q "^  bramble .*RP2040 and RP2350 (Arm" || { echo "protosim is built without Bramble: git submodule update --init libraries/bramble; make"; exit 2; }

if [ -n "$PICO_SDK_PATH" ]; then
  header "build the examples"
  if "$EX/build.sh" > "$TMP/build.log" 2>&1; then pass "examples built for the RP2040 and RP2350"; else fail "examples do not build"; tail -20 "$TMP/build.log"; exit 1; fi
fi
O="$EX/out/rp2040"
[ -f "$O/uart_echo.elf" ] || { echo "no examples in $O: set PICO_SDK_PATH to build them"; exit 2; }

# ─────────────────────────────────────────────────────────────────────────────
header "RP2040: UART0 from a string, and the target chosen by the file"
run echo "$O/uart_echo.elf" --uart0-in $'hello pico\r' --run-ms 100
log_has echo "an Arm ELF file runs on Bramble, as an RP2040 at 125 MHz" "[BRAMBLE] RP2040, Cortex-M0+ at 125 MHz"
has echo "the firmware speaks on UART0" "uart_echo: ready"
has echo "what --uart0-in sends arrives, and the answer comes back" "> HELLO PICO"

header "RP2040: UART0 on a pseudo terminal"
rm -f /tmp/simavr-uart0
"$SIM" "$O/uart_echo.elf" > "$TMP/pty.log" 2>&1 & SIMPID=$!
for i in $(seq 1 50); do [ -e /tmp/simavr-uart0 ] && break; sleep 0.1; done
if [ -e /tmp/simavr-uart0 ]; then
  PTS="$(readlink -f /tmp/simavr-uart0)"
  stty -F "$PTS" raw -echo 2>/dev/null
  exec 3<>"$PTS"
  sleep 0.5; printf 'over the line\r' >&3
  timeout 2 cat <&3 > "$TMP/pty.uart" 2>/dev/null
  exec 3>&-
  kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null; SIMPID=
  has pty "the line is linked at /tmp/simavr-uart0, and a line typed there is answered" "> OVER THE LINE"
else
  fail "no /tmp/simavr-uart0"; kill $SIMPID 2>/dev/null; SIMPID=
fi

header "RP2040: a pin, its duty cycle, and sleep"
run blink "$O/blink_sleep.elf" --pin-log GP25 --pin-duty GP25:500 --run-ms 1000
log_has blink "--pin-log sees the LED go on" "[PIN] GP25=1 at 0.2"
log_has blink "--pin-duty: on 100 ms of every 500" "[DUTY] GP25 20.0% at 500.000 ms"
log_has blink "sleep_ms() sleeps the core (WFE until the alarm)" "[SLEEP] asleep 99.9"
run blinkw "$O/blink_sleep.elf" --pin-log GP25 --pin-duty GP25:500 --run-ms 1000 --warp
if cmp -s "$TMP/blink.uart" "$TMP/blinkw.uart" && diff <(grep '^\[\(PIN\|DUTY\|SLEEP\)' "$TMP/blink.log") <(grep '^\[\(PIN\|DUTY\|SLEEP\)' "$TMP/blinkw.log") > /dev/null; then
  pass "--warp: the same UART output, pin changes, duty cycles and sleep share"
else
  fail "--warp changes what the run shows"
fi
run blinkl "$O/blink_sleep.elf" --run-ms 60000 --warp
has blinkl "--warp: a minute of blinking in a moment" "blink 119"

header "RP2040: both cores"
run mc "$O/multicore.elf" --pin-log GP15 --run-ms 50
log_has mc "core 1 is started by multicore_launch_core1()" "[CORE1] Launched by firmware"
has mc "core 1 answers through the FIFO" "core 1 is up: 0xc0de0001"
has mc "core 1 computes, core 0 prints" "5 squared on core 1: 25"
[ "$(count mc '[PIN] GP15=')" = 5 ] && pass "core 1 toggles GP15 for each number: 5 changes" || fail "GP15 changed $(count mc '[PIN] GP15=') times, not 5"

header "RP2040: inputs: edges and voltages"
run in "$O/inputs.elf" --warp --pin-wave GP2:10 --adc 0:1650 --adc 0:3000@250 --adc temp:706 --run-ms 450
has in "--pin-wave: 10 rising edges in 100 ms, counted in an interrupt" "100 ms: 10 edges"
has in "--adc 0:1650 reads back as 1650 mV" "200 ms: 20 edges, adc0 1650 mV"
has in "--adc 0:3000@250 changes it at 250 ms" "300 ms: 30 edges, adc0 2999 mV"
has in "--adc temp: the temperature sensor" "temp 705 mV"

header "RP2040: the watchdog"
run wd "$O/watchdog.elf" --warp --run-ms 1000
log_has wd "a watchdog that is not fed resets the chip 100 ms after the last feed" "[RST] watchdog reset at 250.2"
has wd "after the reset the chip knows it was the watchdog" "watchdog: reset by the watchdog, 1 so far"
has wd "the scratch registers are kept through a watchdog reset" "watchdog: reset by the watchdog, 3 so far"
has wd "and the program stops when it has seen three" "watchdog: that is enough"
run wdn "$O/watchdog.elf" --run-ms 1000
if cmp -s "$TMP/wd.uart" "$TMP/wdn.uart" && [ "$(grep -c '^\[RST\]' "$TMP/wd.log")" = "$(grep -c '^\[RST\]' "$TMP/wdn.log")" ]; then
  pass "--warp stops at the watchdog: the same resets and output without it"
else
  fail "the watchdog run differs with --warp"
fi

header "options a target does not serve"
"$SIM" "$O/uart_echo.elf" --reset-at 5 --run-ms 1 > "$TMP/no1.log" 2>&1; echo $? > "$TMP/no1.rc"
grep -q "ERROR: --reset-at is not served on bramble" "$TMP/no1.log" && [ "$(cat $TMP/no1.rc)" != 0 ] && pass "--reset-at stops with an error on Bramble" || fail "--reset-at is not refused"
"$SIM" "$O/uart_echo.elf" --soft-uart GP4,GP5 --run-ms 1 > "$TMP/no2.log" 2>&1
grep -q "ERROR: --soft-uart is not served on bramble" "$TMP/no2.log" && pass "--soft-uart stops with an error on Bramble" || fail "--soft-uart is not refused"

header "RP2350 (Cortex-M33): experimental"
R="$EX/out/rp2350"
if [ -f "$R/uart_echo.elf" ]; then
  run r50 "$R/uart_echo.elf" --uart0-in $'hi there\r' --run-ms 100
  log_has r50 "an RP2350 image is known by its IMAGE_DEF block" "[BRAMBLE] RP2350, Cortex-M33 at 150 MHz"
  has r50 "the Pico SDK starts and the UART echoes (IT blocks, RAM code and veneers fixed here)" "> HI THERE"
fi

printf '\n%d passed, %d failed\n' $PASS $FAIL
[ $FAIL -eq 0 ]
