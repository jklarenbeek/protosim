#!/bin/bash
set -e

PROTODUINO_SRC="../../protoduino"
LIB_DIR="libraries/protoduino"

if [ -d "$LIB_DIR" ]; then
    echo "Protoduino already set up in $LIB_DIR"
    exit 0
fi

if [ -d "$PROTODUINO_SRC" ]; then
    echo "Found local protoduino repo, symlinking..."
    mkdir -p libraries
    ln -s "$(realpath --relative-to=libraries $PROTODUINO_SRC)" "$LIB_DIR"
else
    echo "Cloning protoduino..."
    mkdir -p libraries
    git submodule add https://github.com/jklarenbeek/protoduino.git "$LIB_DIR"
fi

echo "Protoduino setup complete."
