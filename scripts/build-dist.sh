#!/usr/bin/env bash
# scripts/build-dist.sh
#
# Build SIREN firmware for all supported targets and copy artifacts to
# the firmware/dist/<target>/ subdirectory for each one.
#
# Usage:
#   bash scripts/build-dist.sh           # build all targets
#   bash scripts/build-dist.sh v3        # build Heltec V3 only
#   bash scripts/build-dist.sh v4        # build Heltec V4 only
#
# Requirements:
#   - PlatformIO CLI (pio) on PATH
#   - Run from the repository root, or from the firmware/ directory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="$REPO_ROOT/firmware"

cd "$FIRMWARE_DIR"

# Resolve pio binary — prefer the PlatformIO venv on Windows
if command -v pio &>/dev/null; then
  PIO=pio
elif [ -f "/c/Users/$USERNAME/.platformio/penv/Scripts/pio.exe" ]; then
  PIO="/c/Users/$USERNAME/.platformio/penv/Scripts/pio.exe"
elif [ -f "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  echo "ERROR: pio not found. Install PlatformIO: https://docs.platformio.org/en/latest/core/installation/index.html"
  exit 1
fi

# ---- helper -----------------------------------------------------------

build_target() {
  local env="$1"
  local out_dir="$2"

  echo ""
  echo "================================================================"
  echo "  Building: $env"
  echo "  Output:   firmware/dist/$out_dir/"
  echo "================================================================"

  mkdir -p "dist/$out_dir"

  # Build OTA binary
  "$PIO" run -e "$env"

  local build_dir=".pio/build/$env"

  # OTA binary (app only, for /update)
  cp "$build_dir/firmware.bin" "dist/$out_dir/${env}.bin"
  echo "  -> dist/$out_dir/${env}.bin (OTA)"

  # Full-flash merged binary (bootloader + partition table + app)
  if [ -f "$build_dir/firmware-merged.bin" ]; then
    cp "$build_dir/firmware-merged.bin" "dist/$out_dir/${env}-full-flash.bin"
    echo "  -> dist/$out_dir/${env}-full-flash.bin (full flash)"
  else
    echo "  NOTE: merged binary not found — run 'pio run -e $env -t mergebin' to generate it"
  fi
}

# ---- targets ----------------------------------------------------------

FILTER="${1:-all}"

case "$FILTER" in
  v3|all)
    build_target "SIREN_v3_room_server" "heltec_v3"
    ;;
esac

case "$FILTER" in
  v4|all)
    build_target "SIREN_v4_room_server" "heltec_v4"
    ;;
esac

echo ""
echo "================================================================"
echo "  Done. Artifacts:"
ls -lh dist/heltec_v3/*.bin 2>/dev/null || true
ls -lh dist/heltec_v4/*.bin 2>/dev/null || true
echo "================================================================"
