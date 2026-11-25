#!/bin/bash
# Checks out Bramble, the RP2040 and RP2350 emulator, at the pinned revision.
# `make` builds it into protosim (build/bramble/); nothing to build here.
# Linux and macOS; the Windows build leaves it out.
set -e
BRAMBLE_DIR="libraries/bramble"
if [ ! -f "$BRAMBLE_DIR/src/cpu.c" ]; then
    echo "Bramble not found, initializing the submodule..."
    git submodule update --init "$BRAMBLE_DIR" || true
fi
if [ -f "$BRAMBLE_DIR/src/cpu.c" ]; then
    echo "Bramble is at $(git -C "$BRAMBLE_DIR" log --oneline -1 2>/dev/null)."
else
    echo "Bramble could not be checked out: protosim builds without the RP2040 and RP2350."
fi
