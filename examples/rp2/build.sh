#!/bin/bash
# build.sh [rp2040|rp2350 ...]: the examples, with the Pico SDK, into out/<chip>/
#   PICO_SDK_PATH=<path to pico-sdk> examples/rp2/build.sh
# Needs cmake and arm-none-eabi-gcc. Without picotool the SDK makes .elf files
# and no .uf2; protosim runs both.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$PICO_SDK_PATH" ] || { echo "set PICO_SDK_PATH to a checkout of the Pico SDK (2.x)"; exit 2; }
for chip in ${@:-rp2040 rp2350}; do
  case $chip in
    rp2040) board=pico;  platform=rp2040 ;;
    rp2350) board=pico2; platform=rp2350-arm-s ;;
    *) echo "unknown chip $chip"; exit 2 ;;
  esac
  b="$HERE/out/build-$chip"; mkdir -p "$b"
  cmake -S "$HERE" -B "$b" -DPICO_BOARD=$board -DPICO_PLATFORM=$platform > "$b/configure.log" 2>&1 || { tail -20 "$b/configure.log"; exit 1; }
  make -C "$b" -j"$(nproc 2>/dev/null || echo 4)" > "$b/build.log" 2>&1 || { tail -20 "$b/build.log"; exit 1; }
  mkdir -p "$HERE/out/$chip"; cp "$b"/*.elf "$HERE/out/$chip/"
  for f in "$b"/*.uf2; do [ -f "$f" ] && cp "$f" "$HERE/out/$chip/"; done
  echo "$chip: $(cd "$HERE/out/$chip" && ls | tr '\n' ' ')"
done
