"""
SIREN Web Client — Flask backend.

Serves:
  REST API  /api/*
  WebSocket /ws
  Static    /*  (React bundle built into static/)

Security notes (ARCHITECTURE.md §6):
  - Binds to 127.0.0.1 only (configurable, but not 0.0.0.0 by default).
  - Serial port path validated against enumerated allow-list.
  - Text length clamped to firmware MAX_TEXT_LEN=160.
  - Room passwords never logged.
  - WS Origin must match the local app.
"""

import json
import logging
import os
import re
import threading
import time
import urllib.parse
import uuid
from typing import Optional

from flask import Flask, jsonify, request, send_from_directory
from flask_sock import Sock

import serial_bridge as bridge_module
import state as state_module
from state import MAX_TEXT_LEN, AppState, Channel, Message, get_state
from serial_bridge import SerialBridge, get_bridge, init_bridge

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# App factory

def create_app(host: str = "127.0.0.1", port: int = 8760, ssl: bool = False) -> Flask:
    app = Flask(__name__, static_folder="static", static_url_path="")
    app.config["SOCK_SERVER_OPTIONS"] = {"ping_interval": 25}
    app.config["_HOST"] = host
    app.config["_PORT"] = port
    app.config["_SSL"] = ssl

    sock = Sock(app)

    # -----------------------------------------------------------------------
    # WebSocket clients registry

    _ws_clients: list = []
    _ws_lock = threading.Lock()

    def broadcast(event: dict) -> None:
        """Send a JSON event to all connected WebSocket clients."""
        msg = json.dumps(event)
        dead = []
        with _ws_lock:
            clients = list(_ws_clients)
        for ws in clients:
            try:
                ws.send(msg)
            except Exception:
                dead.append(ws)
        if dead:
            with _ws_lock:
                for d in dead:
                    try:
                        _ws_clients.remove(d)
                    except ValueError:
                        pass

    # Initialise bridge (no serial port open yet)
    st = get_state()
    bridge = init_bridge(st, broadcast)

    # -----------------------------------------------------------------------
    # Helpers

    def _allowed_origin(origin: Optional[str], host: str, port: int, ssl_mode: bool) -> bool:
        """Return True if the Origin is the local app or missing (same-origin).

        In SSL/remote mode (ssl_mode=True) also accepts any https:// origin whose
        port matches the configured port (scheme+port check; host is not validated
        because binding to 0.0.0.0 means the real client IP is unknown).
        """
        if not origin:
            return True
        # Always allow local origins (both http and https variants)
        local_allowed = {
            f"http://{host}:{port}",
            f"http://localhost:{port}",
            f"http://127.0.0.1:{port}",
            f"https://{host}:{port}",
            f"https://localhost:{port}",
            f"https://127.0.0.1:{port}",
        }
        if origin in local_allowed:
            return True
        # In remote SSL mode: accept https:// origins whose port matches
        if ssl_mode:
            parsed = urllib.parse.urlparse(origin)
            if parsed.scheme == "https" and parsed.port == port:
                return True
        return False

    def _validate_text(text) -> Optional[str]:
        """Strip, validate and clamp text. Returns cleaned text or None on error."""
        if not isinstance(text, str):
            return None
        text = text.strip()
        if not text:
            return None
        return text[:MAX_TEXT_LEN]

    def _validate_pubkey_prefix(prefix) -> bool:
        """12 lowercase hex chars."""
        return isinstance(prefix, str) and bool(re.fullmatch(r'[0-9a-f]{12}', prefix))

    def _validate_channel_idx(idx) -> bool:
        return isinstance(idx, int) and 0 <= idx <= 255

    def _channel_id_from_path(channel_id: str) -> Optional[Channel]:
        return st.channels.get(channel_id)

    # -----------------------------------------------------------------------
    # REST endpoints

    @app.route("/api/ports", methods=["GET"])
    def api_ports():
        """GET /api/ports — list enumerated serial ports."""
        return jsonify({"ports": bridge.list_ports()})

    @app.route("/api/connect", methods=["POST"])
    def api_connect():
        """POST /api/connect — open serial connection."""
        data = request.get_json(silent=True) or {}
        path = data.get("path")
        baud = data.get("baud", 115200)

        if not path:
            return jsonify({"error": "path is required"}), 400

        # Validate path against allow-list of enumerated ports
        allowed = {p["path"] for p in bridge.list_ports()}
        if path not in allowed:
            return jsonify({"error": f"Port '{path}' is not in the enumerated allow-list"}), 400

        if not isinstance(baud, int) or baud <= 0:
            return jsonify({"error": "baud must be a positive integer"}), 400

        ok = bridge.connect(path, baud)
        if not ok:
            return jsonify({"state": st.conn_state, "error": "Failed to connect to device"}), 503

        return jsonify({
            "state": st.conn_state,
            "self": st.self_user.to_api() if st.self_user else None,
        })

    @app.route("/api/disconnect", methods=["POST"])
    def api_disconnect():
        """POST /api/disconnect — close serial connection."""
        bridge.disconnect()
        return jsonify({"state": st.conn_state})

    @app.route("/api/state", methods=["GET"])
    def api_state():
        """GET /api/state — full state snapshot."""
        return jsonify(st.snapshot())

    @app.route("/api/channels/<path:channel_id>/messages", methods=["GET"])
    def api_channel_messages(channel_id: str):
        """GET /api/channels/:id/messages?limit=100"""
        ch = _channel_id_from_path(channel_id)
        if not ch:
            return jsonify({"error": "Channel not found"}), 404

        try:
            limit = int(request.args.get("limit", 100))
            limit = max(1, min(limit, MAX_TEXT_LEN * 10))  # sanity clamp
        except (ValueError, TypeError):
            return jsonify({"error": "limit must be an integer"}), 400

        msgs = list(st.messages.get(channel_id, []))
        return jsonify({"messages": [m.to_api() for m in msgs[-limit:]]})

    @app.route("/api/channels/<path:channel_id>/join", methods=["POST"])
    def api_channel_join(channel_id: str):
        """POST /api/channels/:id/join — join/open a channel."""
        ch = _channel_id_from_path(channel_id)
        if not ch:
            return jsonify({"error": "Channel not found"}), 404

        if not bridge.is_connected():
            return jsonify({"ok": False, "error": "Not connected to device"}), 503

        data = request.get_json(silent=True) or {}
        password = data.get("password", "")

        with st.lock():
            ch.joined = True
            ch.unread = 0

        if ch.kind == "room":
            # Room login is async — send the login command; result arrives via WS
            if password:
                # Never log the password
                bridge.send_login(ch.pubkey_prefix, password)
            # Return immediately; login result arrives via WS 'login' event
            return jsonify({"ok": True, "channel": ch.to_api()})

        return jsonify({"ok": True, "channel": ch.to_api()})

    @app.route("/api/channels/<path:channel_id>/part", methods=["POST"])
    def api_channel_part(channel_id: str):
        """POST /api/channels/:id/part — leave/close a channel."""
        ch = _channel_id_from_path(channel_id)
        if not ch:
            return jsonify({"error": "Channel not found"}), 404

        with st.lock():
            ch.joined = False

        return jsonify({"ok": True})

    @app.route("/api/channels/<path:channel_id>/messages", methods=["POST"])
    def api_send_message(channel_id: str):
        """POST /api/channels/:id/messages — send a message."""
        ch = _channel_id_from_path(channel_id)
        if not ch:
            return jsonify({"error": "Channel not found"}), 404

        data = request.get_json(silent=True) or {}
        text = _validate_text(data.get("text"))
        if text is None:
            return jsonify({"error": "text must be a non-empty string (max 160 chars)"}), 400

        if not bridge.is_connected():
            return jsonify({"ok": False, "error": "Not connected to device"}), 503

        msg: Optional[Message] = None

        if ch.kind in ("dm", "room"):
            if not ch.pubkey_prefix:
                return jsonify({"error": "Channel has no pubkey_prefix"}), 500
            msg = bridge.send_dm(ch.pubkey_prefix, text)
        elif ch.kind == "channel":
            if ch.channel_idx is None:
                return jsonify({"error": "Channel has no channel_idx"}), 500
            msg = bridge.send_channel_msg(ch.channel_idx, text)
        else:
            return jsonify({"error": "Unknown channel kind"}), 400

        if msg is None:
            return jsonify({"ok": False, "error": "Failed to send message"}), 503

        # Store the pending message in history
        with st.lock():
            st.upsert_message(msg)

        return jsonify({"ok": True, "message": msg.to_api()}), 201

    @app.route("/api/advert", methods=["POST"])
    def api_advert():
        """POST /api/advert — send self-advertisement."""
        data = request.get_json(silent=True) or {}
        flood = bool(data.get("flood", False))
        name = data.get("name")

        if name is not None:
            name = _validate_text(name)
            if name is None:
                return jsonify({"error": "name must be a non-empty string"}), 400

        if not bridge.is_connected():
            return jsonify({"ok": False, "error": "Not connected to device"}), 503

        if name is not None:
            bridge.set_advert_name(name)
        bridge.send_advert(flood=flood)
        return jsonify({"ok": True})

    # -----------------------------------------------------------------------
    # WebSocket endpoint

    @sock.route("/ws")
    def ws_endpoint(ws):
        """WebSocket /ws — push-only: server sends events to client."""
        # Origin check — reject non-local origins
        origin = request.headers.get("Origin")
        h = app.config["_HOST"]
        p = app.config["_PORT"]
        s = app.config["_SSL"]
        if not _allowed_origin(origin, h, p, s):
            log.warning("Rejected WS upgrade from non-local origin: %s", origin)
            return  # flask-sock will close the connection

        with _ws_lock:
            _ws_clients.append(ws)

        # Send current state snapshot on connect
        try:
            ws.send(json.dumps({"type": "conn", "state": st.conn_state}))
            ws.send(json.dumps({
                "type": "channels",
                "channels": [c.to_api() for c in st.channels.values()],
            }))
            ws.send(json.dumps({
                "type": "users",
                "users": [u.to_api() for u in st.users.values()],
            }))
        except Exception:
            pass

        # Keep alive (client→server messages are ignored per API contract v1)
        try:
            while True:
                data = ws.receive(timeout=60)
                if data is None:
                    break
                # Ignore all client messages in v1
        except Exception:
            pass
        finally:
            with _ws_lock:
                try:
                    _ws_clients.remove(ws)
                except ValueError:
                    pass

    # -----------------------------------------------------------------------
    # Serve React frontend (production build)

    @app.route("/", defaults={"path": ""})
    @app.route("/<path:path>")
    def serve_frontend(path: str):
        static_dir = app.static_folder
        if path and os.path.exists(os.path.join(static_dir, path)):
            return send_from_directory(static_dir, path)
        return send_from_directory(static_dir, "index.html")

    return app


# ---------------------------------------------------------------------------
# Entry point

if __name__ == "__main__":
    import argparse

    logging.basicConfig(level=logging.INFO)

    parser = argparse.ArgumentParser(description="SIREN web client bridge server")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Bind address (default: 127.0.0.1). "
                             "WARNING: binding to 0.0.0.0 exposes the mesh API with no auth.")
    parser.add_argument("--port", type=int, default=8760, help="Port (default: 8760)")
    parser.add_argument("--ssl-cert", default=None,
                        help="Path to SSL certificate file (PEM). Enables HTTPS when combined with --ssl-key.")
    parser.add_argument("--ssl-key", default=None,
                        help="Path to SSL private key file (PEM). Enables HTTPS when combined with --ssl-cert.")
    args = parser.parse_args()

    ssl_context = None
    if args.ssl_cert and args.ssl_key:
        if not os.path.isfile(args.ssl_cert):
            print(f"ERROR: SSL certificate file not found: {args.ssl_cert}")
            raise SystemExit(1)
        if not os.path.isfile(args.ssl_key):
            print(f"ERROR: SSL key file not found: {args.ssl_key}")
            raise SystemExit(1)
        ssl_context = (args.ssl_cert, args.ssl_key)

    use_ssl = ssl_context is not None
    scheme = "https" if use_ssl else "http"

    app = create_app(host=args.host, port=args.port, ssl=use_ssl)
    print(f"SIREN bridge listening on {scheme}://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, ssl_context=ssl_context)
