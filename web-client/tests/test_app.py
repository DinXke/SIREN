"""
pytest tests for the SIREN web client Flask backend.

Covers:
  - /api/ports  (happy path)
  - /api/state  (happy path, disconnected)
  - /api/connect (bad path — port not in allow-list → 400)
  - /api/channels/:id/messages GET (not found → 404, empty → 200)
  - /api/channels/:id/messages POST (text validation: empty → 400, too long → clamped)
  - /api/channels/:id/join (not connected → 503)
  - /api/advert (not connected → 503)
  - Serial frame framing helpers (unit tests, no hardware)
  - State helpers (unit tests)
"""

import json
import struct
import sys
import os
import uuid

import pytest

# ---- add server/ to path so imports work without install
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'server'))

import state as state_module
from state import AppState, Channel, Message, User, ADV_TYPE_CHAT, ADV_TYPE_ROOM
import serial_bridge as bridge_module
from serial_bridge import _write_frame, _read_frame


# ---------------------------------------------------------------------------
# Fixtures

@pytest.fixture(autouse=True)
def reset_state(monkeypatch):
    """Reset module-level singletons before each test."""
    fresh_state = AppState()
    monkeypatch.setattr(state_module, '_state', fresh_state)
    monkeypatch.setattr(bridge_module, '_bridge', None)
    yield fresh_state


@pytest.fixture
def app(reset_state):
    # Patch list_ports so no real serial hardware is needed
    import app as app_module
    from app import create_app
    from serial_bridge import SerialBridge

    def fake_list_ports(self):
        return [
            {"path": "/dev/ttyUSB0", "label": "Fake SIREN device"},
            {"path": "COM5", "label": "Fake SIREN device (Windows)"},
        ]

    import serial_bridge
    original = SerialBridge.list_ports
    SerialBridge.list_ports = fake_list_ports

    application = create_app(host="127.0.0.1", port=8760)
    application.config["TESTING"] = True

    yield application

    SerialBridge.list_ports = original


@pytest.fixture
def client(app):
    return app.test_client()


# ---------------------------------------------------------------------------
# /api/ports

def test_get_ports_returns_list(client):
    resp = client.get("/api/ports")
    assert resp.status_code == 200
    data = resp.get_json()
    assert "ports" in data
    assert isinstance(data["ports"], list)
    assert any(p["path"] == "/dev/ttyUSB0" for p in data["ports"])


# ---------------------------------------------------------------------------
# /api/state

def test_state_disconnected(client):
    resp = client.get("/api/state")
    assert resp.status_code == 200
    data = resp.get_json()
    assert data["conn"] == "disconnected"
    assert data["self"] is None
    assert data["channels"] == []
    assert data["users"] == []


# ---------------------------------------------------------------------------
# /api/connect

def test_connect_rejects_unknown_port(client):
    resp = client.post("/api/connect", json={"path": "/dev/evil_injection"})
    assert resp.status_code == 400
    data = resp.get_json()
    assert "error" in data
    assert "allow-list" in data["error"]


def test_connect_missing_path(client):
    resp = client.post("/api/connect", json={})
    assert resp.status_code == 400
    assert "path" in resp.get_json()["error"]


def test_connect_bad_baud(client):
    resp = client.post("/api/connect", json={"path": "/dev/ttyUSB0", "baud": -1})
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# /api/disconnect

def test_disconnect_when_already_disconnected(client):
    resp = client.post("/api/disconnect")
    assert resp.status_code == 200
    assert resp.get_json()["state"] == "disconnected"


# ---------------------------------------------------------------------------
# /api/channels/:id/messages  GET

def test_channel_messages_not_found(client):
    resp = client.get("/api/channels/dm:aabbccddeeff/messages")
    assert resp.status_code == 404


def test_channel_messages_empty(client, reset_state):
    ch = Channel(
        id="dm:aabbccddeeff",
        kind="dm",
        name="alice",
        display_name="alice",
        locked=False,
        pubkey_prefix="aabbccddeeff",
    )
    reset_state.channels["dm:aabbccddeeff"] = ch
    resp = client.get("/api/channels/dm:aabbccddeeff/messages")
    assert resp.status_code == 200
    assert resp.get_json()["messages"] == []


def test_channel_messages_limit(client, reset_state):
    ch = Channel(
        id="chan:0",
        kind="channel",
        name="general",
        display_name="#general",
        locked=False,
        channel_idx=0,
    )
    reset_state.channels["chan:0"] = ch
    for i in range(10):
        msg = Message(
            id=str(uuid.uuid4()),
            channel_id="chan:0",
            from_name="alice",
            self_msg=False,
            text=f"msg {i}",
            ts=1000 + i,
            status="confirmed",
        )
        reset_state.upsert_message(msg)

    resp = client.get("/api/channels/chan:0/messages?limit=3")
    assert resp.status_code == 200
    msgs = resp.get_json()["messages"]
    assert len(msgs) == 3
    assert msgs[-1]["text"] == "msg 9"


# ---------------------------------------------------------------------------
# /api/channels/:id/messages  POST (send)

def test_send_message_not_connected(client, reset_state):
    ch = Channel(
        id="dm:aabbccddeeff",
        kind="dm",
        name="alice",
        display_name="alice",
        locked=False,
        pubkey_prefix="aabbccddeeff",
    )
    reset_state.channels["dm:aabbccddeeff"] = ch
    resp = client.post("/api/channels/dm:aabbccddeeff/messages", json={"text": "hello"})
    assert resp.status_code == 503


def test_send_message_empty_text(client, reset_state):
    ch = Channel(
        id="dm:aabbccddeeff",
        kind="dm",
        name="alice",
        display_name="alice",
        locked=False,
        pubkey_prefix="aabbccddeeff",
    )
    reset_state.channels["dm:aabbccddeeff"] = ch
    resp = client.post("/api/channels/dm:aabbccddeeff/messages", json={"text": "   "})
    assert resp.status_code == 400


def test_send_message_channel_not_found(client):
    resp = client.post("/api/channels/dm:000000000000/messages", json={"text": "hello"})
    assert resp.status_code == 404


# ---------------------------------------------------------------------------
# /api/channels/:id/join

def test_join_not_connected(client, reset_state):
    ch = Channel(
        id="room:aabbccddeeff",
        kind="room",
        name="ops",
        display_name="#siren-ops",
        locked=True,
        pubkey_prefix="aabbccddeeff",
    )
    reset_state.channels["room:aabbccddeeff"] = ch
    resp = client.post("/api/channels/room:aabbccddeeff/join", json={"password": "secret"})
    assert resp.status_code == 503


def test_join_channel_not_found(client):
    resp = client.post("/api/channels/room:000000000000/join", json={})
    assert resp.status_code == 404


# ---------------------------------------------------------------------------
# /api/advert

def test_advert_not_connected(client):
    resp = client.post("/api/advert", json={"flood": False})
    assert resp.status_code == 503


def test_advert_empty_name_rejected(client):
    resp = client.post("/api/advert", json={"name": "  "})
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# Frame framing unit tests (no hardware)

class FakeSerial:
    """Minimal in-memory serial port for testing frame encoding/decoding."""

    def __init__(self):
        self._buf = bytearray()

    def write(self, data: bytes) -> int:
        self._buf.extend(data)
        return len(data)

    @property
    def in_waiting(self) -> int:
        return len(self._buf)

    def read(self, n: int) -> bytes:
        chunk = bytes(self._buf[:n])
        del self._buf[:n]
        return chunk

    def get_written(self) -> bytes:
        return bytes(self._buf)


def test_write_frame_encoding():
    fs = FakeSerial()
    payload = b'\x01\x02\x03'
    _write_frame(fs, payload)
    written = fs.get_written()
    assert written[0:1] == b'<'
    assert struct.unpack_from('<H', written, 1)[0] == 3
    assert written[3:] == payload


def test_write_frame_too_large():
    fs = FakeSerial()
    with pytest.raises(ValueError, match="too large"):
        _write_frame(fs, bytes(200))  # MAX_FRAME_SIZE = 176


def test_read_frame_two_consecutive_frames():
    """Verify that two back-to-back frames are each read correctly in sequence."""
    p1 = b'\x05hello'
    p2 = b'\x10world'
    raw = b'>' + struct.pack('<H', len(p1)) + p1 + b'>' + struct.pack('<H', len(p2)) + p2

    fs = FakeSerial()
    fs._buf.extend(raw)

    buf = bytearray()
    r1 = _read_frame(fs, buf, [0])
    r2 = _read_frame(fs, buf, [0])
    assert r1 == p1
    assert r2 == p2


def test_read_frame_with_fake_serial():
    """Simulate the device sending a frame and verifying we parse it correctly."""
    payload = b'\x05hello'
    frame = b'>' + struct.pack('<H', len(payload)) + payload

    # Build a FakeSerial preloaded with the frame
    fs = FakeSerial()
    fs._buf.extend(frame)

    buf = bytearray()
    result = _read_frame(fs, buf, [0])
    assert result == payload


def test_read_frame_ignores_garbage_before_start():
    payload = b'\x02\x03'
    frame = b'GARBAGE>' + struct.pack('<H', len(payload)) + payload

    fs = FakeSerial()
    fs._buf.extend(frame)

    buf = bytearray()
    result = _read_frame(fs, buf, [0])
    assert result == payload


def test_read_frame_incomplete_returns_none():
    # Only send the header, not the payload
    fs = FakeSerial()
    fs._buf.extend(b'>' + struct.pack('<H', 10))  # says 10 bytes coming but only 3 total

    buf = bytearray()
    result = _read_frame(fs, buf, [0])
    assert result is None


# ---------------------------------------------------------------------------
# State unit tests

def test_upsert_message_insert():
    st = AppState()
    msg = Message(
        id="m1",
        channel_id="dm:aabb",
        from_name="alice",
        self_msg=False,
        text="hello",
        ts=1000,
        status="confirmed",
    )
    st.upsert_message(msg)
    q = st.get_or_create_messages("dm:aabb")
    assert len(q) == 1
    assert q[0].text == "hello"


def test_upsert_message_update_status():
    st = AppState()
    msg = Message(
        id="m1",
        channel_id="dm:aabb",
        from_name="",
        self_msg=True,
        text="hi",
        ts=1000,
        status="pending",
    )
    st.upsert_message(msg)
    updated = Message(
        id="m1",
        channel_id="dm:aabb",
        from_name="",
        self_msg=True,
        text="hi",
        ts=1000,
        status="confirmed",
    )
    st.upsert_message(updated)
    q = st.get_or_create_messages("dm:aabb")
    assert len(q) == 1
    assert q[0].status == "confirmed"


def test_rebuild_channels_from_users():
    st = AppState()
    room_user = User(
        pubkey_prefix="aabbccddeeff",
        pubkey_full=bytes(32),
        name="ops",
        adv_type=ADV_TYPE_ROOM,
    )
    chat_user = User(
        pubkey_prefix="112233445566",
        pubkey_full=bytes(32),
        name="alice",
        adv_type=ADV_TYPE_CHAT,
    )
    st.users["aabbccddeeff"] = room_user
    st.users["112233445566"] = chat_user

    st.rebuild_channels_from_users()

    assert "room:aabbccddeeff" in st.channels
    assert st.channels["room:aabbccddeeff"].kind == "room"
    assert st.channels["room:aabbccddeeff"].locked is True
    assert "dm:112233445566" in st.channels
    assert st.channels["dm:112233445566"].kind == "dm"


def test_user_to_api():
    u = User(
        pubkey_prefix="aabbccddeeff",
        pubkey_full=bytes(32),
        name="ops",
        adv_type=ADV_TYPE_ROOM,
        last_seen=9999,
    )
    api = u.to_api()
    assert api["pubkeyPrefix"] == "aabbccddeeff"
    assert api["isRoom"] is True
    assert api["lastSeen"] == 9999


def test_channel_to_api():
    ch = Channel(
        id="chan:0",
        kind="channel",
        name="general",
        display_name="#general",
        locked=False,
        joined=True,
        unread=3,
        channel_idx=0,
    )
    api = ch.to_api()
    assert api["id"] == "chan:0"
    assert api["kind"] == "channel"
    assert api["displayName"] == "#general"
    assert api["locked"] is False
    assert api["joined"] is True
    assert api["unread"] == 3
