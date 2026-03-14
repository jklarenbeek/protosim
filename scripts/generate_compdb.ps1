<#
.SYNOPSIS
Generates compile_commands.json for both Windows host and PlatformIO firmware.
#>

$ErrorActionPreference = "Stop"

$workspaceDir = (Get-Item ".\").FullName
$binDir = Join-Path $workspaceDir "bin"
$pioDir = Join-Path $workspaceDir ".pio"

if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir | Out-Null
}

$pioCompileCommands = Join-Path $pioDir "build\compile_commands.json"
$outputFile = Join-Path $binDir "compile_commands.json"

$pioCmd = "pio"
if (Get-Command "pio" -ErrorAction SilentlyContinue) {
    # already in PATH
} elseif (Test-Path "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe") {
    $pioCmd = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
} else {
    Write-Warning "pio command not found! Make sure PlatformIO is installed."
    exit 1
}

Write-Host "Generating compile_commands.json for PlatformIO firmware..."
& $pioCmd run -t compiledb

# Generate Windows host entries
$hostEntries = @()

$cflags = "-Wall -O2 -include ./include/win32_compat.h -DPROTOSIM_BUILD=1 -I$workspaceDir/libraries/simavr/simavr/sim -I$workspaceDir/libraries/simavr/simavr/sim/avr -I$workspaceDir/include"
$cc = "gcc"

foreach ($src in @("src/protosim.c", "src/uart_com.c")) {
    $file = Join-Path $workspaceDir $src
    # Replace backslashes with forward slashes for Clangd compatibility, unless it's Windows specific. Using standard format.
    $obj = $src -replace '\.c$', '.o'
    
    $entry = @{
        directory = $workspaceDir.Replace("\", "/")
        command = "$cc $cflags -c $file -o $obj"
        file = $file.Replace("\", "/")
    }
    $hostEntries += $entry
}

$finalEntries = @()

if (Test-Path $pioCompileCommands) {
    Write-Host "Merging PlatformIO and Host entries..."
    $pioJson = Get-Content $pioCompileCommands -Raw | ConvertFrom-Json
    if ($pioJson) {
        $finalEntries += $pioJson
    }
}

$finalEntries += $hostEntries

$finalEntries | ConvertTo-Json -Depth 10 | Set-Content $outputFile

Write-Host "Successfully written to $outputFile"
