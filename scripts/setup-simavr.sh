#!/bin/bash
set -e

SIMAVR_DIR="libraries/simavr"

if [ ! -d "$SIMAVR_DIR" ]; then
    echo "Cloning simavr..."
    mkdir -p libraries
    git submodule add https://github.com/buserror/simavr.git "$SIMAVR_DIR"
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
