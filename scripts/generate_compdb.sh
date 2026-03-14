#!/bin/bash
# Generates compile_commands.json for Linux/WSL host and PlatformIO firmware.

set -e

WORKSPACE_DIR=$(pwd)
BIN_DIR="$WORKSPACE_DIR/bin"
PIO_DIR="$WORKSPACE_DIR/.pio"
PIO_COMPILE_COMMANDS="$PIO_DIR/build/compile_commands.json"
OUTPUT_FILE="$BIN_DIR/compile_commands.json"

mkdir -p "$BIN_DIR"

if command -v pio &> /dev/null; then
    PIO_CMD="pio"
elif [ -f "$HOME/.platformio/penv/bin/pio" ]; then
    PIO_CMD="$HOME/.platformio/penv/bin/pio"
else
    echo "pio command not found! Make sure PlatformIO is installed."
    exit 1
fi

echo "Generating compile_commands.json for PlatformIO firmware..."
"$PIO_CMD" run -t compiledb

echo "Generating Linux host entries..."

HOST_ENTRIES="["
CC="gcc"
CFLAGS="-Wall -O2 -I./include -I./libraries/simavr/simavr/sim -I./libraries/simavr/simavr/sim/avr"

first=true
for src in "src/protosim.c" "src/uart_pty.c"; do
    if [ "$first" = true ]; then
        first=false
    else
        HOST_ENTRIES="$HOST_ENTRIES,"
    fi
    file="$WORKSPACE_DIR/$src"
    obj="${src%.c}.o"
    HOST_ENTRIES="$HOST_ENTRIES
  {
    \"directory\": \"$WORKSPACE_DIR\",
    \"command\": \"$CC $CFLAGS -c $file -o $obj\",
    \"file\": \"$file\"
  }"
done
HOST_ENTRIES="$HOST_ENTRIES
]"

if [ -f "$PIO_COMPILE_COMMANDS" ]; then
    echo "Merging PlatformIO and Host entries..."
    # A simple way to merge JSON arrays using jq (if installed) or python. 
    # Since PIO generates an array and we have an array, we can use python.
    python3 -c "
import json
with open('$PIO_COMPILE_COMMANDS', 'r') as f:
    pio = json.load(f)
host = json.loads('''$HOST_ENTRIES''')
with open('$OUTPUT_FILE', 'w') as f:
    json.dump(pio + host, f, indent=4)
"
else
    echo "PlatformIO compile_commands.json not found. Saving only Host entries."
    echo "$HOST_ENTRIES" > "$OUTPUT_FILE"
fi

echo "Successfully written to $OUTPUT_FILE"
