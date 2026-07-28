#!/usr/bin/env bash
# SIREN Web Client — install and start (Linux / macOS)
# Usage: ./install.sh [--port 8760] [--host 127.0.0.1]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="127.0.0.1"
PORT="8760"

# Parse optional args
while [[ $# -gt 0 ]]; do
  case $1 in
    --port) PORT="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

echo "=== SIREN Web Client installer ==="

# ---- Python virtualenv + deps ----
VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
  echo "[1/4] Creating Python virtual environment..."
  python3 -m venv "$VENV_DIR"
fi

echo "[2/4] Installing Python dependencies..."
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet -r "$SCRIPT_DIR/server/requirements.txt"

# ---- Node / frontend ----
FRONTEND_DIR="$SCRIPT_DIR/frontend"
STATIC_DIR="$SCRIPT_DIR/server/static"

if [ -d "$FRONTEND_DIR" ] && [ -f "$FRONTEND_DIR/package.json" ]; then
  echo "[3/4] Building React frontend..."
  cd "$FRONTEND_DIR"
  npm ci --silent
  npm run build --silent
  # Vite outputs to frontend/dist by default; copy to server/static
  mkdir -p "$STATIC_DIR"
  cp -r dist/. "$STATIC_DIR/"
  cd "$SCRIPT_DIR"
else
  echo "[3/4] No frontend found — skipping build."
  mkdir -p "$STATIC_DIR"
  # Create a minimal placeholder so Flask can serve something
  if [ ! -f "$STATIC_DIR/index.html" ]; then
    cat > "$STATIC_DIR/index.html" <<'HTML'
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>SIREN Web Client</title></head>
<body>
  <h1>SIREN Web Client</h1>
  <p>Frontend not built yet. The REST API and WebSocket bridge are running.</p>
  <p>API: <a href="/api/state">/api/state</a></p>
</body>
</html>
HTML
  fi
fi

# ---- Start server ----
echo "[4/4] Starting SIREN bridge on http://${HOST}:${PORT} ..."
echo ""
echo "  Open http://${HOST}:${PORT} in your browser"
echo "  Press Ctrl+C to stop."
echo ""

exec "$VENV_DIR/bin/python" "$SCRIPT_DIR/server/app.py" --host "$HOST" --port "$PORT"
