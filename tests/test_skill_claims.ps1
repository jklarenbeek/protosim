# =============================================================================
# test_skill_claims.ps1 — Verify all claims in SKILL.md
# =============================================================================

$ROOT       = Split-Path $PSScriptRoot -Parent
$SIM        = Join-Path $ROOT "bin\protosim.exe"
$BUILD_DIR  = Join-Path $ROOT "build"
if (-not (Test-Path $BUILD_DIR)) { New-Item -ItemType Directory -Path $BUILD_DIR }

$DEMO_SRC   = Join-Path $ROOT "examples\demo\demo.c"
$DEMO_ELF   = Join-Path $BUILD_DIR "demo.elf"
$BOOTLOADER = Join-Path $ROOT "libraries\simavr\examples\board_simduino\ATmegaBOOT_168_atmega328.ihex"

$PASS_COUNT = 0
$FAIL_COUNT = 0

function print_header($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Yellow }
function print_pass($msg)   { Write-Host "  [PASS] $msg" -ForegroundColor Green; $script:PASS_COUNT++ }
function print_fail($msg)   { Write-Host "  [FAIL] $msg" -ForegroundColor Red;   $script:FAIL_COUNT++ }
function print_info($msg)   { Write-Host "  [INFO] $msg" -ForegroundColor Cyan }

function assert_match([string]$out, [string]$label, [string]$pattern) {
    if ($out -match $pattern) { print_pass $label }
    else                      { print_fail "$label  [pattern: $pattern]" }
}

# 1. TEST COMPILATION CLAIM
print_header "Claim 1: Firmware Compilation (avr-gcc -g)"
if (Test-Path $DEMO_ELF) { Remove-Item $DEMO_ELF }
$comp_out = & avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -g -o $DEMO_ELF $DEMO_SRC 2>&1
if ($LASTEXITCODE -eq 0 -and (Test-Path $DEMO_ELF)) {
    print_pass "Compiled $DEMO_ELF using avr-gcc"
} else {
    print_fail "Failed to compile $DEMO_ELF`n$comp_out"
    exit 1
}

# 2. TEST MAX-STEPS CLAIM
print_header "Claim 2: --max-steps limits simulation"
$o = & $SIM $DEMO_ELF --max-steps 10000 2>&1 | Out-String
assert_match $o "Max-steps reached" "MAX-STEPS \(10000\) reached"

# 3. TEST DEBUG FEATURES (Breakpoint + Dump Regs + r24 check)
print_header "Claim 3: Debugging (-b, --dump-regs)"
# In demo.c, main calls compute(20). 20 is 0x14.
# We need ~400k cycles to hit compute, so 1M steps is safe.
$o = & $SIM $DEMO_ELF -b compute --dump-regs --max-steps 1000000 2>&1 | Out-String
assert_match $o "Breakpoint hit" "BREAKPOINT HIT: compute"
assert_match $o "Register dump appears" "Register Dump @ PC="
assert_match $o "r24 contains argument (0x14)" "r24\s+=\s+0x14"

# 4. TEST WATCHES
print_header "Claim 4: Watches (-w)"
# We watch SRAM 0x0100 while compute is hit.
$o = & $SIM $DEMO_ELF -w 0x0100:1:test_var -b compute --max-steps 1000000 2>&1 | Out-String
assert_match $o "Watch reported" "test_var\s+@\s+SRAM\[0x0100\]"

# 5. TEST PROFILING FEATURES
print_header "Claim 5: Profiling (--coverage, --profile, --callgraph)"
$o = & $SIM $DEMO_ELF --coverage --profile --callgraph --max-steps 1000000 2>&1 | Out-String
assert_match $o "Coverage report" "Code Coverage Report"
assert_match $o "Flat profile report" "Flat Performance Profile"
assert_match $o "Call graph report" "Call Graph"
assert_match $o "Hot path reported" "Hot Path"

# 6. TEST UART INTERACTION (TCP on Windows)
print_header "Claim 6: Serial UART Interaction (TCP 127.0.0.1:4000)"
$sim_process = Start-Process -FilePath $SIM -ArgumentList "$DEMO_ELF --max-steps 50000000" -NoNewWindow -PassThru
print_info "Started protosim PID: $($sim_process.Id)"

$client = $null
$connected = $false
for ($i = 0; $i -lt 20; $i++) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4000)
        $connected = $true
        break
    } catch {
        Start-Sleep -Milliseconds 200
    }
}

if ($connected) {
    try {
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $line = $reader.ReadLine()
        if ($line -match "protosim-demo ready") {
            print_pass "Received banner over TCP: $line"
        } else {
            print_fail "Did not receive expected banner. Got: $line"
        }
        $client.Close()
    } catch {
        print_fail "Error during communication: $($_.Exception.Message)"
    }
} else {
    print_fail "Failed to connect to TCP port 4000 after retries."
}

if (-not $sim_process.HasExited) { 
    Stop-Process -Id $sim_process.Id -Force 
    print_info "Stopped protosim"
}

# 7. TEST BOOTLOADER (Structural claim)
print_header "Claim 7: --bootloader structural support"
$o = & $SIM $DEMO_ELF --bootloader $BOOTLOADER --max-steps 1000 2>&1 | Out-String
assert_match $o "Bootloader HEX loaded" "\[BL\] Loaded bootloader HEX"
assert_match $o "Initial PC redirected" "\[BL\] Initial PC set to bootloader entry"

# SUMMARY
$total = $PASS_COUNT + $FAIL_COUNT
Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Yellow
$color = if ($FAIL_COUNT -eq 0) { "Green" } else { "Red" }
Write-Host "  RESULTS: $PASS_COUNT / $total passed   ($FAIL_COUNT failed)" -ForegroundColor $color
Write-Host ("=" * 60) -ForegroundColor Yellow

if ($FAIL_COUNT -gt 0) { exit 1 } else { exit 0 }
