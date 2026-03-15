#!/bin/bash
set -e

PROTODUINO_SRC="../../protoduino"
LIB_DIR="libraries/protoduino"

if [ -f "$LIB_DIR/src/protoduino.h" ]; then
    echo "Protoduino already set up in $LIB_DIR"
    exit 0
fi

if [ -d "$PROTODUINO_SRC" ]; then
    echo "Found local protoduino repo, symlinking..."
    mkdir -p libraries
    ln -s "$(realpath --relative-to=libraries $PROTODUINO_SRC)" "$LIB_DIR"
else
    echo "Protoduino not found or incomplete, initializing submodules..."
    git submodule update --init --recursive "$LIB_DIR" || true
    if [ ! -f "$LIB_DIR/src/protoduino.h" ]; then
        echo "Still not found, attempting clone..."
        mkdir -p libraries
        rm -rf "$LIB_DIR"
        git clone https://github.com/jklarenbeek/protoduino.git "$LIB_DIR"
    fi
fi

echo "Protoduino setup complete."
