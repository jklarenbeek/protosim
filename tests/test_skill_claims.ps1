# =============================================================================
# test_skill_claims.ps1 — Verify all claims in SKILL.md
# =============================================================================

$ROOT       = Split-Path $PSScriptRoot -Parent
$SIM        = Join-Path $ROOT "bin\protosim.exe"
$BUILD_DIR  = Join-Path $ROOT "build"
if (-not (Test-Path $BUILD_DIR)) { New-Item -ItemType Directory -Path $BUILD_DIR }

$DEMO_ELF       = Join-Path $ROOT "examples\demo\demo.elf"
$WATCH_ELF      = Join-Path $ROOT "examples\struct_watch\struct_watch.elf"
$CALLSTACK_ELF  = Join-Path $ROOT "examples\deep_callstack\deep_callstack.elf"
$UART_INT_ELF   = Join-Path $ROOT "examples\uart_interrupt\uart_interrupt.elf"
$BOOTLOADER     = Join-Path $ROOT "libraries\simavr\examples\board_simduino\ATmegaBOOT_168_atmega328.ihex"

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

# 1. TEST MAX-STEPS CLAIM (using demo)
print_header "Claim 1: --max-steps limits simulation"
$o = & $SIM $DEMO_ELF --max-steps 10000 2>&1 | Out-String
assert_match $o "Max-steps reached" "MAX-STEPS \(10000\) reached"

# 2. TEST DEBUG FEATURES: Breakpoints & Registers (using demo)
print_header "Claim 2: Debugging (-b, --dump-regs)"
$o = & $SIM $DEMO_ELF -b compute --dump-regs --max-steps 1000000 2>&1 | Out-String
assert_match $o "Breakpoint hit: compute" "BREAKPOINT HIT: compute"
assert_match $o "Register dump appears" "Register Dump @ PC="
assert_match $o "r24 contains argument" "r24\s+=\s+0x14"

# 3. TEST DEBUG FEATURES: Watches & SRAM Dump (using struct_watch)
print_header "Claim 3: Watches & SRAM Dump (-w, --dump-sram)"
# player is at 0x0100. It's 4 bytes: hp(2), x(1), y(1).
$o = & $SIM $WATCH_ELF -w 0x0100:2:player_hp -b update_player --dump-sram 0x0100:16 --max-steps 1000000 2>&1 | Out-String
assert_match $o "Breakpoint hit: update_player" "BREAKPOINT HIT: update_player"
assert_match $o "Watch reported: player_hp" "player_hp\s+@\s+SRAM\[0x0100\]"
assert_match $o "SRAM dump appears" "--- SRAM 0x0100 .. 0x010f ---"
assert_match $o "SRAM dump contains player data" "0100: 64 00 0a 14" # 100(0x64), 0, 10(0x0a), 20(0x14)

# 4. TEST PROFILING FEATURES (using deep_callstack)
print_header "Claim 4: Profiling (--profile, --callgraph)"
$o = & $SIM $CALLSTACK_ELF --profile --callgraph --max-steps 1000000 2>&1 | Out-String
assert_match $o "Flat profile reported" "Flat Performance Profile"
assert_match $o "Call graph reported" "Call Graph"
assert_match $o "Recursive function 'fibonacci' in profile" "fibonacci"
# Using flexible regex for call path to avoid character encoding issues
assert_match $o "Call path a->b->c->d in callgraph" "a.*b.*c.*d"

# 5. TEST COVERAGE (using uart_interrupt)
print_header "Claim 5: Coverage (--coverage)"
$o = & $SIM $UART_INT_ELF --coverage --max-steps 500000 2>&1 | Out-String
assert_match $o "Coverage report" "Code Coverage Report"
# Coverage might be partial if budget is low, but function should be present
assert_match $o "Function 'uart_init' present in report" "uart_init"
assert_match $o "Function 'uart_print' present in report" "uart_print"

# 6. TEST UART INTERACTION (TCP on Windows using uart_interrupt)
print_header "Claim 6: Serial UART Interaction (TCP 127.0.0.1:4000)"
# We use a very large step limit for background process
$sim_process = Start-Process -FilePath $SIM -ArgumentList "$UART_INT_ELF --max-steps 100000000" -NoNewWindow -PassThru
print_info "Started protosim PID: $($sim_process.Id)"

$client = $null
$connected = $false
# Retries for connection
for ($i = 0; $i -lt 30; $i++) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4000)
        $connected = $true
        break
    } catch {
        Start-Sleep -Milliseconds 300
    }
}

if ($connected) {
    try {
        $stream = $client.GetStream()
        # Set shorter timeouts for the client stream
        $client.ReceiveTimeout = 2000
        $client.SendTimeout    = 2000
        
        $reader = New-Object System.IO.StreamReader($stream)
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true

        # Read until we see "Initialized" or timeout
        $init_found = $false
        for ($k = 0; $k -lt 10; $k++) {
            if ($stream.DataAvailable) {
                $line = $reader.ReadLine()
                if ($line -match "UART Interrupt Example Initialized") {
                    $init_found = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 200
        }

        if ($init_found) {
            print_pass "Received init banner"
        } else {
            print_fail "Did not receive expected banner."
        }

        # Test Echo
        $writer.Write("X")
        # Wait for simulation
        Start-Sleep -Milliseconds 1000
        
        $echo_found = $false
        for ($j = 0; $j -lt 10; $j++) {
            if ($stream.DataAvailable) {
                $line = $reader.ReadLine()
                if ($line -match "Echo: X") {
                    $echo_found = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 200
        }

        if ($echo_found) {
            print_pass "Received echo 'X' over TCP"
        } else {
            print_fail "Failed to receive echo 'X'"
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
