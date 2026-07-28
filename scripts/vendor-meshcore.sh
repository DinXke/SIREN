#!/usr/bin/env bash
# scripts/vendor-meshcore.sh — Update the vendored MeshCore firmware to a new upstream tag.
#
# Usage:
#   ./scripts/vendor-meshcore.sh <upstream-tag>
#
# Examples:
#   ./scripts/vendor-meshcore.sh room-server-v1.16.0
#   ./scripts/vendor-meshcore.sh room-server-v1.17.0
#
# The script:
#   1. Fetches the tag from the 'meshcore-upstream' remote
#   2. Extracts the upstream tree into firmware/
#   3. Restores all SIREN-specific files that are not in upstream
#   4. Appends the SIREN platformio.ini section back to the ini file
#   5. Commits the result and creates an annotated tag base-<upstream-tag>
#
# Designed so that upgrading to a future release (e.g. v1.17) is a single command.
# See docs/meshcore-upgrade-runbook.md for conflict resolution guidance.

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
REMOTE="meshcore-upstream"
PREFIX="firmware"

# SIREN-specific subdirectories inside firmware/ that are NOT in upstream.
# These are preserved across every upgrade automatically.
SIREN_EXTRA_DIRS=(
  "examples/siren_room_server"
)

# The platformio.ini file that needs its SIREN section re-appended after upgrade.
SIREN_INI_PATH="$PREFIX/variants/heltec_v3/platformio.ini"

# Marker that identifies the start of the SIREN-specific section in platformio.ini.
SIREN_INI_MARKER="; ===  SIREN —"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
TAG="${1:-}"
if [ -z "$TAG" ]; then
  echo "Usage: $0 <upstream-tag>"
  echo "  e.g. $0 room-server-v1.16.0"
  exit 1
fi

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "ERROR: Working tree has uncommitted changes. Commit or stash first."
  git status --short --untracked-files=no
  exit 1
fi

echo "==> Fetching $REMOTE ..."
git fetch "$REMOTE" --tags --quiet

# Verify the tag exists
if ! git rev-parse "refs/tags/$TAG" >/dev/null 2>&1; then
  echo "ERROR: Tag '$TAG' not found in remote '$REMOTE'."
  echo "Available tags:"
  git tag | grep -E "room-server|repeater|companion" | sort -V | tail -10
  exit 1
fi

UPSTREAM_COMMIT=$(git rev-list -n 1 "$TAG")
PREV_BASE=$(git describe --tags --match 'base-*' HEAD 2>/dev/null || echo "none")

echo "==> Upgrading from $PREV_BASE  -->  $TAG ($UPSTREAM_COMMIT)"
echo ""

# ---------------------------------------------------------------------------
# Step 1: Save SIREN-specific extra directories
# ---------------------------------------------------------------------------
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

echo "==> Saving SIREN-specific files ..."
for relpath in "${SIREN_EXTRA_DIRS[@]}"; do
  src="$PREFIX/$relpath"
  if [ -e "$src" ]; then
    destdir="$WORKDIR/extras/$(dirname "$relpath")"
    mkdir -p "$destdir"
    cp -r "$src" "$destdir/"
    echo "    saved: $src"
  else
    echo "    (not found, skipping): $src"
  fi
done

# ---------------------------------------------------------------------------
# Step 2: Save the SIREN-specific platformio.ini section
# ---------------------------------------------------------------------------
SIREN_INI_SECTION=""
if [ -f "$SIREN_INI_PATH" ] && grep -q "$SIREN_INI_MARKER" "$SIREN_INI_PATH"; then
  # Extract everything from the marker line onward
  SIREN_INI_SECTION=$(sed -n "/$SIREN_INI_MARKER/,\$p" "$SIREN_INI_PATH")
  echo "    saved: SIREN platformio.ini section ($(echo "$SIREN_INI_SECTION" | wc -l) lines)"
fi

# ---------------------------------------------------------------------------
# Step 3: Replace firmware/ with the upstream tag tree
# ---------------------------------------------------------------------------
echo ""
echo "==> Replacing $PREFIX/ with upstream $TAG ..."
git rm -r --quiet --cached "$PREFIX/"
rm -rf "$PREFIX/"
mkdir "$PREFIX"

# git archive extracts the tag's root tree (which IS the firmware root)
git archive "$TAG" | tar -x -C "$PREFIX/"
echo "    extracted $(find "$PREFIX" -type f | wc -l) files from $TAG"

# ---------------------------------------------------------------------------
# Step 4: Restore SIREN-specific extra directories
# ---------------------------------------------------------------------------
echo ""
echo "==> Restoring SIREN-specific files ..."
for relpath in "${SIREN_EXTRA_DIRS[@]}"; do
  src="$WORKDIR/extras/$(basename "$relpath")"
  dst="$PREFIX/$relpath"
  if [ -e "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -r "$src" "$dst"
    echo "    restored: $dst"
  fi
done

# ---------------------------------------------------------------------------
# Step 5: Restore SIREN platformio.ini section
# ---------------------------------------------------------------------------
if [ -n "$SIREN_INI_SECTION" ] && [ -f "$SIREN_INI_PATH" ]; then
  # Check whether the section is already present (idempotency guard)
  if ! grep -q "$SIREN_INI_MARKER" "$SIREN_INI_PATH"; then
    printf '\n%s\n' "$SIREN_INI_SECTION" >> "$SIREN_INI_PATH"
    echo "    restored: SIREN platformio.ini section"
  else
    echo "    SIREN platformio.ini section already present (skipped)"
  fi
fi

# ---------------------------------------------------------------------------
# Step 6: Stage all changes
# ---------------------------------------------------------------------------
echo ""
echo "==> Staging changes ..."
git add "$PREFIX/"

echo ""
git diff --stat --cached | tail -5

# ---------------------------------------------------------------------------
# Step 7: Commit
# ---------------------------------------------------------------------------
COMMIT_MSG="chore: vendor MeshCore $TAG

Upstream commit: $UPSTREAM_COMMIT
Previous base:   $PREV_BASE

SIREN-specific files preserved:
$(for d in "${SIREN_EXTRA_DIRS[@]}"; do echo "  - firmware/$d"; done)
  - SIREN section in firmware/variants/heltec_v3/platformio.ini

Co-Authored-By: Paperclip <noreply@paperclip.ing>"

git commit -m "$COMMIT_MSG"
echo ""
echo "==> Committed!"

# ---------------------------------------------------------------------------
# Step 8: Annotated tag
# ---------------------------------------------------------------------------
BASE_TAG="base-$TAG"
git tag -a "$BASE_TAG" -m "Vendor MeshCore $TAG

Upstream commit: $UPSTREAM_COMMIT
Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "==> Tagged: $BASE_TAG"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo "  Upgrade complete: $BASE_TAG"
echo "============================================================"
echo ""
echo "Next steps:"
echo "  1. Verify the build:  pio run -e SIREN_v3_room_server"
echo "  2. Run Phase 0 check: flash%, RAM%, no linker errors"
echo "  3. Push:              git push origin multiroom --follow-tags"
echo ""
echo "See docs/meshcore-upgrade-runbook.md for conflict resolution."
