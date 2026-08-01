#!/usr/bin/env bash
# scripts/build-dist.sh
#
# Build SIREN firmware for all supported targets and copy artifacts to
# the firmware/dist/<target>/ subdirectory for each one.
#
# Also generates dist/version.json with version string + SHA-256 of each
# OTA binary so nodes can verify integrity before flashing.
#
# Usage:
#   bash scripts/build-dist.sh           # build all targets + version.json
#   bash scripts/build-dist.sh v3        # build Heltec V3 only
#   bash scripts/build-dist.sh v4        # build Heltec V4 only
#
# Requirements:
#   - PlatformIO CLI (pio) on PATH
#   - python3 (for SHA-256 computation and version.json generation)
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

# Resolve python3
if command -v python3 &>/dev/null; then
  PYTHON=python3
elif command -v python &>/dev/null; then
  PYTHON=python
else
  echo "ERROR: python3 not found (needed for SHA-256 generation)"
  exit 1
fi

# ---- helper: compute SHA-256 hex of a file ----------------------------

sha256_of() {
  $PYTHON -c "import sys,hashlib; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"
}

# ---- helper: build one target -----------------------------------------

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

  # OTA binary (app only, for /update and GitHub self-update)
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

SHA256_V3=""
SHA256_V4=""

case "$FILTER" in
  v3|all)
    build_target "SIREN_v3_room_server" "heltec_v3"
    SHA256_V3=$(sha256_of "dist/heltec_v3/SIREN_v3_room_server.bin")
    echo "  SHA-256 V3: $SHA256_V3"
    ;;
esac

case "$FILTER" in
  v4|all)
    build_target "SIREN_v4_room_server" "heltec_v4"
    SHA256_V4=$(sha256_of "dist/heltec_v4/SIREN_v4_room_server.bin")
    echo "  SHA-256 V4: $SHA256_V4"
    ;;
esac

# ---- generate dist/version.json ---------------------------------------
# Only write when both targets were built (full build).  Single-target
# builds update only the relevant SHA-256 field if version.json already
# exists, otherwise skip.

BUILD_DATE=$(date -u +%Y-%m-%d)

# Extract FIRMWARE_VERSION from V3 platformio.ini (source of truth)
FIRMWARE_VERSION=$(grep 'FIRMWARE_VERSION=' variants/heltec_v3/platformio.ini \
  | head -1 | sed "s/.*FIRMWARE_VERSION='\"\\(.*\\)\"'.*/\\1/")
if [ -z "$FIRMWARE_VERSION" ]; then
  FIRMWARE_VERSION="unknown"
fi

write_version_json() {
  local v3_sha="$1"
  local v4_sha="$2"
  local out="dist/version.json"
  $PYTHON - <<PYEOF
import json, sys
d = {
    "version": "${FIRMWARE_VERSION}",
    "date": "${BUILD_DATE}",
    "v3": {"sha256": "${v3_sha}"},
    "v4": {"sha256": "${v4_sha}"}
}
with open("${out}", "w") as f:
    json.dump(d, f, indent=2)
    f.write("\n")
print("  -> ${out}")
PYEOF
}

case "$FILTER" in
  all)
    write_version_json "$SHA256_V3" "$SHA256_V4"
    ;;
  v3)
    # Patch only the v3 sha256 if version.json exists; create skeleton otherwise
    if [ -f "dist/version.json" ]; then
      $PYTHON - <<PYEOF
import json
with open("dist/version.json","r") as f: d = json.load(f)
d.setdefault("v3",{})["sha256"] = "${SHA256_V3}"
with open("dist/version.json","w") as f: json.dump(d,f,indent=2); f.write("\n")
print("  -> dist/version.json (v3 sha256 updated)")
PYEOF
    else
      write_version_json "$SHA256_V3" "BUILD_V4_FIRST"
    fi
    ;;
  v4)
    if [ -f "dist/version.json" ]; then
      $PYTHON - <<PYEOF
import json
with open("dist/version.json","r") as f: d = json.load(f)
d.setdefault("v4",{})["sha256"] = "${SHA256_V4}"
with open("dist/version.json","w") as f: json.dump(d,f,indent=2); f.write("\n")
print("  -> dist/version.json (v4 sha256 updated)")
PYEOF
    else
      write_version_json "BUILD_V3_FIRST" "$SHA256_V4"
    fi
    ;;
esac

echo ""
echo "================================================================"
echo "  Done. Artifacts:"
ls -lh dist/heltec_v3/*.bin 2>/dev/null || true
ls -lh dist/heltec_v4/*.bin 2>/dev/null || true
ls -lh dist/version.json    2>/dev/null || true
echo "================================================================"
