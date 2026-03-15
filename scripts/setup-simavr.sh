#!/bin/bash
set -e

SIMAVR_DIR="libraries/simavr"

if [ ! -f "$SIMAVR_DIR/simavr/sim/sim_avr.h" ]; then
    echo "simavr not found or incomplete, initializing submodules..."
    git submodule update --init --recursive "$SIMAVR_DIR" || true
    if [ ! -f "$SIMAVR_DIR/simavr/sim/sim_avr.h" ]; then
        echo "Still not found, attempting clone..."
        mkdir -p libraries
        rm -rf "$SIMAVR_DIR"
        git clone https://github.com/buserror/simavr.git "$SIMAVR_DIR"
    fi
fi

echo "Building simavr..."
cd "$SIMAVR_DIR"

if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]] || uname -s | grep -qiE "MINGW|MSYS|CYGWIN"; then
    echo "Windows environment detected. Running custom build script..."
    cd ../..
    cmd.exe /c "scripts\build-simavr-win.bat"
else
    # Build only the core library on Linux (skips examples/tests that need GLUT)
    make -C simavr
fi
echo "simavr built successfully."
