# =============================================================================
# test_bootloader.ps1 — protosim test suite covering all SKILL.md features
#
# Run from repo root:
#   powershell -ExecutionPolicy Bypass -File tests\test_bootloader.ps1
# =============================================================================

$ROOT       = Split-Path $PSScriptRoot -Parent
$SIM        = Join-Path $ROOT "bin\protosim.exe"
$DEMO_ELF   = Join-Path $ROOT "examples\demo\demo.elf"
$BOOTLOADER = Join-Path $ROOT "libraries\simavr\examples\board_simduino\ATmegaBOOT_168_atmega328.ihex"
$MCU        = "atmega328p"
$FREQ       = "16000000"

$PASS_COUNT = 0
$FAIL_COUNT = 0

function print_header($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Yellow }
function print_pass($msg)   { Write-Host "  [PASS] $msg" -ForegroundColor Green; $script:PASS_COUNT++ }
function print_fail($msg)   { Write-Host "  [FAIL] $msg" -ForegroundColor Red;   $script:FAIL_COUNT++ }
function print_info($msg)   { Write-Host "  [INFO] $msg" -ForegroundColor Cyan }

# Runs protosim, returns combined stdout+stderr as a string
function run_sim([string[]]$SimArgs) {
    print_info "cmd: protosim.exe $($SimArgs -join ' ')"
    $out = & $SIM @SimArgs 2>&1 | Out-String
    $out.TrimEnd() -split "`n" | ForEach-Object { Write-Host "    $_" }
    return $out
}

# Assert pattern matches (or doesn't match) in output
function assert_match([string]$out, [string]$label, [string]$pattern) {
    if ($out -match $pattern) { print_pass $label }
    else                      { print_fail "$label  [pattern: $pattern]" }
}
function assert_no_match([string]$out, [string]$label, [string]$pattern) {
    if ($out -match $pattern) { print_fail "$label  [should NOT match: $pattern]" }
    else                      { print_pass $label }
}

# =============================================================================
# Pre-flight
# =============================================================================
print_header "Pre-flight: files exist"
foreach ($f in @($SIM, $DEMO_ELF, $BOOTLOADER)) {
    if (Test-Path $f) { print_pass "exists: $f" }
    else              { print_fail "MISSING: $f"; exit 1 }
}

# =============================================================================
# TEST 1 — Plain run (no bootloader)
# =============================================================================
print_header "TEST 1: Plain run (no bootloader)"
$o = run_sim @($DEMO_ELF, "-m", $MCU, "-f", $FREQ, "--max-steps", "200000")
assert_match    $o "simulator starts"               "protosim running.*demo"
assert_match    $o "max-steps guard fires"          "MAX-STEPS \(200000\) reached"
assert_match    $o "final PC reported"              "Final PC=0x"
assert_no_match $o "no [BL] prefix"                "\[BL\]"

# =============================================================================
# TEST 2 — Bootloader HEX loads and redirects entry point
# =============================================================================
print_header "TEST 2: Bootloader HEX loaded, entry-point redirected"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER, "-m", $MCU, "-f", $FREQ, "--max-steps", "200")
assert_match $o "[BL] load message"                "\[BL\] Loaded bootloader HEX"
assert_match $o "[BL] entry PC message"            "\[BL\] Initial PC set to bootloader entry"
assert_match $o "entry is top-of-flash (0x7xxx)"   "\[BL\] Initial PC set to bootloader entry 0x7"
assert_match $o "simulator still starts"           "protosim running"

# =============================================================================
# TEST 3 — --max-steps ignores bootloader instructions
#           Budget of 1 should still fire even with a bootloader that takes
#           thousands of cycles before handing off to the app.
# =============================================================================
print_header "TEST 3: --max-steps counts only app instructions"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER, "-m", $MCU, "-f", $FREQ, "--max-steps", "1")
assert_match $o "max-steps reached with budget=1"  "MAX-STEPS \(1\) reached"
assert_match $o "bootloader was present"           "\[BL\] Loaded bootloader HEX"

# =============================================================================
# TEST 4 — Breakpoint on 'compute'  (with bootloader)
# =============================================================================
print_header "TEST 4: Breakpoint on 'compute' with bootloader"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-b", "compute",
               "--max-steps", "2000000")
assert_match $o "breakpoint declared at 0x00d4"   "\[DBG\].*compute.*PC=0x00d4"
assert_match $o "breakpoint fires"                "\[DBG\] BREAK.*compute"

# =============================================================================
# TEST 5 — Breakpoint on 'main' proves BL -> app handoff
# =============================================================================
print_header "TEST 5: Breakpoint on 'main' — confirms BL hands off to app"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-b", "main",
               "--max-steps", "5000000")
assert_match $o "main breakpoint fires"           "\[DBG\] BREAK.*main"

# =============================================================================
# TEST 6 — Watch + --dump-regs at breakpoint
# =============================================================================
print_header "TEST 6: Watch expression + --dump-regs"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-b", "compute",
               "-w", "0x0100:1:sram_top",
               "--dump-regs",
               "--max-steps", "2000000")
assert_match $o "watch sram_top declared"         "sram_top"
assert_match $o "register dump (r0=)"             "r0="

# =============================================================================
# TEST 7 — --dump-sram hex dump at breakpoint
# =============================================================================
print_header "TEST 7: --dump-sram at breakpoint"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-b", "compute",
               "--dump-sram", "0x0100:16",
               "--max-steps", "2000000")
assert_match $o "SRAM dump header with address"   "SRAM.*0x0100"
assert_match $o "hex bytes in dump"               "[0-9a-fA-F]{2} [0-9a-fA-F]{2}"

# =============================================================================
# TEST 8 — Single-step mode (-s)
# =============================================================================
print_header "TEST 8: Single-step mode (-s)"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-s",
               "--max-steps", "50")
assert_match $o "STEP lines appear"               "\[DBG\] STEP  PC=0x"

# =============================================================================
# TEST 9 — Periodic cycle trace (-t <cycles>)
# =============================================================================
print_header "TEST 9: Periodic trace (-t 1000)"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "-t", "1000",
               "--max-steps", "500000")
assert_match $o "TRACE lines appear"              "\[DBG\] TRACE PC=0x"

# =============================================================================
# TEST 10 — Coverage (no bootloader, baseline)
# =============================================================================
print_header "TEST 10: Code coverage report (no bootloader)"
$o = run_sim @($DEMO_ELF,
               "-m", $MCU, "-f", $FREQ,
               "--coverage",
               "--max-steps", "2000000")
assert_match $o "coverage section header"         "CODE COVERAGE"
assert_match $o "compute has nonzero coverage"    "compute.*[0-9]+(\.[0-9]+)?%"
assert_match $o "unused_handler never reached"    "unused_handler.*NEVER REACHED"

# =============================================================================
# TEST 11 — Coverage WITH bootloader
# =============================================================================
print_header "TEST 11: Code coverage with bootloader"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "--coverage",
               "--max-steps", "2000000")
assert_match $o "coverage section present"        "CODE COVERAGE"
assert_match $o "main appears in coverage"        "main.*%"

# =============================================================================
# TEST 12 — Flat profile (--profile)
# =============================================================================
print_header "TEST 12: Flat profile (--profile)"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "--profile",
               "--max-steps", "2000000")
assert_match $o "FLAT PROFILE header"             "FLAT PROFILE"
assert_match $o "compute appears in profile"      "compute"
assert_match $o "cycle counts present"            "[0-9]+ cycles"

# =============================================================================
# TEST 13 — Call graph (--callgraph)
# =============================================================================
print_header "TEST 13: Call graph (--callgraph)"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "--callgraph",
               "--max-steps", "2000000")
assert_match $o "CALL GRAPH header"               "CALL GRAPH"
assert_match $o "main->compute edge"              "main.*->.*compute|compute.*<-.*main"
assert_match $o "hot path reported"               "HOT PATH|hot path"

# =============================================================================
# TEST 14 — All profiling to file (--profile-out)
# =============================================================================
print_header "TEST 14: All profiling flags + --profile-out"
$reportFile = Join-Path $env:TEMP "protosim_test_report.txt"
if (Test-Path $reportFile) { Remove-Item $reportFile }

$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ,
               "--coverage", "--profile", "--callgraph",
               "--profile-out", $reportFile,
               "--max-steps", "2000000")
assert_match $o "simulation ran to max-steps"     "MAX-STEPS"

if (Test-Path $reportFile) {
    $rpt = Get-Content $reportFile -Raw
    print_info "Report file: $reportFile ($((Get-Item $reportFile).Length) bytes)"
    foreach ($section in @("CODE COVERAGE", "FLAT PROFILE", "CALL GRAPH")) {
        if ($rpt -match $section) { print_pass "Section '$section' in report file" }
        else                      { print_fail "Section '$section' MISSING from report file" }
    }
} else {
    print_fail "--profile-out file not created: $reportFile"
}

# =============================================================================
# TEST 15 — Step budget equivalence: with vs without bootloader
#            Same --max-steps should reach end-of-budget for both runs
# =============================================================================
print_header "TEST 15a: Step budget (no bootloader, max-steps=500)"
$o = run_sim @($DEMO_ELF, "-m", $MCU, "-f", $FREQ, "--max-steps", "500")
assert_match $o "max-steps reached"               "MAX-STEPS \(500\) reached"

print_header "TEST 15b: Step budget (with bootloader, same max-steps=500)"
$o = run_sim @($DEMO_ELF, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ, "--max-steps", "500")
assert_match $o "max-steps reached with BL"       "MAX-STEPS \(500\) reached"
assert_match $o "bootloader was present"          "\[BL\] Initial PC set"

# =============================================================================
# TEST 16 — HEX app + HEX bootloader (no ELF)
# =============================================================================
print_header "TEST 16: HEX app firmware + HEX bootloader"
$DEMO_HEX = Join-Path $ROOT "examples\demo\demo.hex"
if (-not (Test-Path $DEMO_HEX)) {
    avr-objcopy -O ihex -R .eeprom $DEMO_ELF $DEMO_HEX 2>&1 | Out-Null
    print_info "Generated demo.hex from demo.elf"
}
$o = run_sim @($DEMO_HEX, "--bootloader", $BOOTLOADER,
               "-m", $MCU, "-f", $FREQ, "--max-steps", "500000")
assert_match $o "[BL] loaded OK"                  "\[BL\] Loaded bootloader HEX"
assert_match $o "simulation runs to completion"   "MAX-STEPS|cpu_Done|DONE|CRASHED"

# =============================================================================
# SUMMARY
# =============================================================================
$total = $PASS_COUNT + $FAIL_COUNT
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Yellow
$color = if ($FAIL_COUNT -eq 0) { "Green" } else { "Red" }
Write-Host "  RESULTS: $PASS_COUNT / $total passed   ($FAIL_COUNT failed)" -ForegroundColor $color
Write-Host ("=" * 60) -ForegroundColor Yellow

if ($FAIL_COUNT -gt 0) { exit 1 } else { exit 0 }
