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
# Build only the core library (skips examples/tests that need GLUT)
make -C simavr
echo "simavr built successfully."
