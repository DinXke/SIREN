"""
MeshCore companion_radio serial frame bridge.

Frame framing (from firmware/src/helpers/ArduinoSerialInterface.cpp):
  Host  → Device:  b'<' + uint16-LE length + payload
  Device → Host:   b'>' + uint16-LE length + payload
  Payload byte 0 is the command / response / push code.

Protocol constants match firmware/examples/companion_radio/MyMesh.cpp.
"""

import logging
import struct
import threading
import time
import uuid
from typing import Callable, Optional

import serial
import serial.tools.list_ports

from state import (
    ADV_TYPE_CHAT,
    ADV_TYPE_ROOM,
    MAX_TEXT_LEN,
    AppState,
    Channel,
    Message,
    User,
    get_state,
)

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Protocol constants (from MyMesh.cpp)
# ---------------------------------------------------------------------------

CMD_APP_START              = 1
CMD_SEND_TXT_MSG           = 2
CMD_SEND_CHANNEL_TXT_MSG   = 3
CMD_GET_CONTACTS           = 4
CMD_SEND_SELF_ADVERT       = 7
CMD_SET_ADVERT_NAME        = 8
CMD_SYNC_NEXT_MESSAGE      = 10
CMD_DEVICE_QUERY           = 22
CMD_SEND_LOGIN             = 26
CMD_GET_CHANNEL            = 31

RESP_CODE_OK                   = 0
RESP_CODE_ERR                  = 1
RESP_CODE_CONTACTS_START       = 2
RESP_CODE_CONTACT              = 3
RESP_CODE_END_OF_CONTACTS      = 4
RESP_CODE_SELF_INFO            = 5
RESP_CODE_SENT                 = 6
RESP_CODE_NO_MORE_MESSAGES     = 10
RESP_CODE_DEVICE_INFO          = 13
RESP_CODE_CONTACT_MSG_RECV_V3  = 16
RESP_CODE_CHANNEL_MSG_RECV_V3  = 17
RESP_CODE_CHANNEL_INFO         = 18

PUSH_CODE_ADVERT            = 0x80
PUSH_CODE_SEND_CONFIRMED    = 0x82
PUSH_CODE_MSG_WAITING       = 0x83
PUSH_CODE_LOGIN_SUCCESS     = 0x85
PUSH_CODE_LOGIN_FAIL        = 0x86
PUSH_CODE_NEW_ADVERT        = 0x8A

PUB_KEY_SIZE   = 32
MAX_FRAME_SIZE = 176

# Firmware app protocol version we advertise (enables V3 message format)
APP_PROTOCOL_VER = 3

TXT_TYPE_PLAIN = 0

# Timeout waiting for synchronous responses
RESPONSE_TIMEOUT_SECS = 5.0

# How long to wait for contacts sync before continuing
CONTACTS_SYNC_TIMEOUT_SECS = 10.0


# ---------------------------------------------------------------------------
# Frame I/O helpers
# ---------------------------------------------------------------------------

def _write_frame(port: serial.Serial, payload: bytes) -> None:
    """Send a frame to the device: '<' + uint16-LE length + payload."""
    if len(payload) > MAX_FRAME_SIZE:
        raise ValueError(f"Frame payload too large: {len(payload)} > {MAX_FRAME_SIZE}")
    header = b'<' + struct.pack('<H', len(payload))
    port.write(header + payload)


def _read_frame(port: serial.Serial, buf: bytearray, pos_ref: list) -> Optional[bytes]:
    """
    Non-blocking incremental frame reader. Reads available bytes from *port*
    into *buf*. When a complete '>' frame is found, returns the payload bytes
    and resets the buffer. Returns None if no complete frame yet.

    pos_ref is a one-element list used as a mutable reference for the current
    parse position (avoids a global state object).
    """
    data = port.read(port.in_waiting or 1)
    buf.extend(data)

    # Parse state machine: look for '>' start byte
    i = 0
    while i < len(buf):
        if buf[i] != ord('>'):
            i += 1
            continue
        # Need at least 3 bytes for header ('>' + 2 bytes length)
        if i + 3 > len(buf):
            break
        length = struct.unpack_from('<H', buf, i + 1)[0]
        frame_end = i + 3 + length
        if frame_end > len(buf):
            break
        payload = bytes(buf[i + 3 : frame_end])
        # Consume up to frame_end
        del buf[:frame_end]
        return payload
    # Discard any leading garbage before the next potential '>'
    next_gt = buf.find(b'>')
    if next_gt > 0:
        del buf[:next_gt]
    return None


# ---------------------------------------------------------------------------
# SerialBridge
# ---------------------------------------------------------------------------

class SerialBridge:
    """
    Owns the serial port connection to the MeshCore companion_radio node.
    Runs a background reader thread that decodes incoming frames and updates
    AppState, broadcasting JSON events to connected WebSocket clients.

    Thread-safety: _write_lock guards all serial writes. AppState uses its own
    lock for state mutations. Callbacks (broadcast_fn) are called from the
    reader thread with no lock held.
    """

    def __init__(self, state: AppState, broadcast_fn: Callable[[dict], None]) -> None:
        self._state = state
        self._broadcast = broadcast_fn

        self._port: Optional[serial.Serial] = None
        self._write_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._reader_thread: Optional[threading.Thread] = None

        # Synchronisation events for blocking connect()
        self._self_info_event = threading.Event()
        self._contacts_done_event = threading.Event()

        # Pending login: channel_id -> threading.Event, result bool
        self._pending_login_events: dict[str, tuple[threading.Event, list]] = {}

    # ------------------------------------------------------------------
    # Public API

    def list_ports(self) -> list[dict]:
        """Return enumerated serial ports as a list of {path, label} dicts."""
        ports = []
        for p in serial.tools.list_ports.comports():
            ports.append({"path": p.device, "label": p.description or p.device})
        return ports

    def connect(self, path: str, baud: int = 115200) -> bool:
        """
        Open serial port and perform the handshake:
          1. CMD_DEVICE_QUERY  → sets app_target_ver=3 on device
          2. CMD_APP_START     → receive RESP_CODE_SELF_INFO
          3. CMD_GET_CONTACTS  → async; contacts arrive in background thread

        Returns True if RESP_SELF_INFO was received within timeout.
        """
        if self._port and self._port.is_open:
            self.disconnect()

        self._self_info_event.clear()
        self._contacts_done_event.clear()
        self._stop_event.clear()

        try:
            port = serial.Serial(path, baud, timeout=0.1)
        except serial.SerialException as exc:
            log.error("Failed to open %s: %s", path, exc)
            with self._state.lock():
                self._state.conn_state = "error"
            return False

        self._port = port

        with self._state.lock():
            self._state.conn_state = "connecting"

        self._broadcast({"type": "conn", "state": "connecting"})

        self._reader_thread = threading.Thread(
            target=self._read_loop, name="serial-reader", daemon=True
        )
        self._reader_thread.start()

        # Step 1: CMD_DEVICE_QUERY — sets app_target_ver=3 on device
        self._send_device_query()

        # Step 2: CMD_APP_START — device responds with RESP_CODE_SELF_INFO
        self._send_app_start()

        # Wait for self_info
        if not self._self_info_event.wait(RESPONSE_TIMEOUT_SECS):
            log.warning("Timeout waiting for RESP_CODE_SELF_INFO")
            with self._state.lock():
                self._state.conn_state = "error"
            self._broadcast({"type": "conn", "state": "error", "error": "No response from device"})
            return False

        # Step 3: Trigger contacts sync (async)
        self._send_get_contacts()

        return True

    def disconnect(self) -> None:
        self._stop_event.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=3)
            self._reader_thread = None
        if self._port and self._port.is_open:
            try:
                self._port.close()
            except Exception:
                pass
        self._port = None
        with self._state.lock():
            self._state.conn_state = "disconnected"
            self._state.self_user = None
        self._broadcast({"type": "conn", "state": "disconnected"})

    def is_connected(self) -> bool:
        return self._port is not None and self._port.is_open and not self._stop_event.is_set()

    # ------------------------------------------------------------------
    # Commands

    def send_dm(self, pubkey_prefix_hex: str, text: str) -> Optional[Message]:
        """
        Send a direct message to a contact identified by 6-byte pubkey prefix.
        Returns a pending Message on success, None on error.
        """
        user = self._state.users.get(pubkey_prefix_hex)
        if not user:
            return None

        text = text.strip()[:MAX_TEXT_LEN]
        if not text:
            return None

        ts = int(time.time())
        msg_id = str(uuid.uuid4())

        # payload: [cmd, txt_type, attempt, ts_u32[4], pubkey_prefix[6], text]
        prefix_bytes = bytes.fromhex(pubkey_prefix_hex)
        payload = bytes([CMD_SEND_TXT_MSG, TXT_TYPE_PLAIN, 0]) + \
                  struct.pack('<I', ts) + \
                  prefix_bytes + \
                  text.encode('utf-8', errors='replace')

        channel_id = f"dm:{pubkey_prefix_hex}"
        if user.adv_type == ADV_TYPE_ROOM:
            channel_id = f"room:{pubkey_prefix_hex}"

        msg = Message(
            id=msg_id,
            channel_id=channel_id,
            from_name="",
            self_msg=True,
            text=text,
            ts=ts,
            status="pending",
        )

        with self._write_lock:
            if not self.is_connected():
                return None
            _write_frame(self._port, payload)

        # We can't easily correlate RESP_CODE_SENT with this message here because
        # RESP_CODE_SENT returns an expected_ack hash (4 bytes), not the msg_id.
        # We store the pending mapping keyed by a placeholder; the read loop will
        # update status to "sent" when RESP_CODE_SENT arrives.
        # For simplicity we track by insertion order: the next RESP_SENT is ours.
        with self._state.lock():
            self._state.pending_msgs[0] = (msg_id, channel_id)  # slot 0 = most recent outbound

        return msg

    def send_channel_msg(self, channel_idx: int, text: str) -> Optional[Message]:
        """Send a group channel message."""
        text = text.strip()[:MAX_TEXT_LEN]
        if not text:
            return None

        ts = int(time.time())
        msg_id = str(uuid.uuid4())

        # payload: [cmd, txt_type=0, channel_idx, ts_u32[4], text]
        payload = bytes([CMD_SEND_CHANNEL_TXT_MSG, TXT_TYPE_PLAIN, channel_idx]) + \
                  struct.pack('<I', ts) + \
                  text.encode('utf-8', errors='replace')

        channel_id = f"chan:{channel_idx}"

        msg = Message(
            id=msg_id,
            channel_id=channel_id,
            from_name="",
            self_msg=True,
            text=text,
            ts=ts,
            status="pending",
        )

        with self._write_lock:
            if not self.is_connected():
                return None
            _write_frame(self._port, payload)

        with self._state.lock():
            self._state.pending_msgs[0] = (msg_id, channel_id)

        return msg

    def send_login(self, pubkey_prefix_hex: str, password: str) -> None:
        """Send CMD_SEND_LOGIN for a room contact."""
        user = self._state.users.get(pubkey_prefix_hex)
        if not user:
            return
        # payload: [cmd, full_pub_key[32], password_string]
        payload = bytes([CMD_SEND_LOGIN]) + \
                  user.pubkey_full + \
                  password.encode('utf-8', errors='replace')
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    def send_advert(self, flood: bool = False) -> None:
        flood_byte = 1 if flood else 0
        payload = bytes([CMD_SEND_SELF_ADVERT, flood_byte])
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    def set_advert_name(self, name: str) -> None:
        payload = bytes([CMD_SET_ADVERT_NAME]) + name.encode('utf-8', errors='replace')
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    def request_channel(self, idx: int) -> None:
        payload = bytes([CMD_GET_CHANNEL, idx])
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    # ------------------------------------------------------------------
    # Internal command senders

    def _send_device_query(self) -> None:
        # CMD_DEVICE_QUERY with app_target_ver = APP_PROTOCOL_VER
        payload = bytes([CMD_DEVICE_QUERY, APP_PROTOCOL_VER])
        with self._write_lock:
            _write_frame(self._port, payload)

    def _send_app_start(self) -> None:
        # CMD_APP_START: [1, reserved*7, app_name...]
        app_name = b"SIREN-WebBridge"
        payload = bytes([CMD_APP_START]) + bytes(7) + app_name
        with self._write_lock:
            _write_frame(self._port, payload)

    def _send_get_contacts(self) -> None:
        payload = bytes([CMD_GET_CONTACTS])
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    def _drain_messages(self) -> None:
        """Send CMD_SYNC_NEXT_MESSAGE in a loop until NO_MORE_MESSAGES."""
        # Called from the read loop — do NOT hold _write_lock across the whole loop
        payload = bytes([CMD_SYNC_NEXT_MESSAGE])
        with self._write_lock:
            if self.is_connected():
                _write_frame(self._port, payload)

    # ------------------------------------------------------------------
    # Background reader thread

    def _read_loop(self) -> None:
        buf = bytearray()
        pos_ref = [0]
        while not self._stop_event.is_set():
            try:
                frame = _read_frame(self._port, buf, pos_ref)
                if frame:
                    self._handle_frame(frame)
            except serial.SerialException as exc:
                log.error("Serial read error: %s", exc)
                break
            except Exception as exc:
                log.exception("Unexpected error in serial read loop: %s", exc)
        # Port was closed or error — update state
        if not self._stop_event.is_set():
            with self._state.lock():
                self._state.conn_state = "error"
            self._broadcast({"type": "conn", "state": "error", "error": "Serial port closed unexpectedly"})

    def _handle_frame(self, payload: bytes) -> None:
        if not payload:
            return
        code = payload[0]

        if code == RESP_CODE_DEVICE_INFO:
            # We just need to receive this; app_target_ver is now set on device
            log.debug("Received RESP_CODE_DEVICE_INFO")

        elif code == RESP_CODE_SELF_INFO:
            self._handle_self_info(payload)

        elif code == RESP_CODE_CONTACTS_START:
            log.debug("Contacts sync started")

        elif code == RESP_CODE_CONTACT:
            self._handle_contact(payload)

        elif code == RESP_CODE_END_OF_CONTACTS:
            self._handle_end_of_contacts()

        elif code == RESP_CODE_SENT:
            self._handle_resp_sent(payload)

        elif code == RESP_CODE_NO_MORE_MESSAGES:
            log.debug("No more queued messages")

        elif code == RESP_CODE_CHANNEL_INFO:
            self._handle_channel_info(payload)

        elif code == RESP_CODE_CONTACT_MSG_RECV_V3:
            self._handle_contact_msg_v3(payload)

        elif code == RESP_CODE_CHANNEL_MSG_RECV_V3:
            self._handle_channel_msg_v3(payload)

        elif code == PUSH_CODE_MSG_WAITING:
            # Device signals there are queued messages — drain them
            self._drain_messages()

        elif code == PUSH_CODE_SEND_CONFIRMED:
            self._handle_send_confirmed(payload)

        elif code == PUSH_CODE_LOGIN_SUCCESS:
            self._handle_login_result(payload, success=True)

        elif code == PUSH_CODE_LOGIN_FAIL:
            self._handle_login_result(payload, success=False)

        elif code in (PUSH_CODE_ADVERT, PUSH_CODE_NEW_ADVERT):
            # New contact appeared on mesh — re-sync contacts
            self._send_get_contacts()

        elif code in (RESP_CODE_OK, RESP_CODE_ERR):
            log.debug("Generic OK/ERR response code=%d", code)

        else:
            log.debug("Unhandled frame code=0x%02x len=%d", code, len(payload))

    # ------------------------------------------------------------------
    # Frame handlers

    def _handle_self_info(self, payload: bytes) -> None:
        """
        RESP_CODE_SELF_INFO layout (from MyMesh.cpp CMD_APP_START handler):
          [5, adv_type, tx_power, max_tx_power, pub_key[32], lat_i32[4], lon_i32[4],
           multi_acks, advert_loc_policy, telemetry_mode, manual_add_contacts,
           freq_u32[4], bw_u32[4], sf, cr, ...name_bytes...]
        """
        if len(payload) < 1 + 1 + 1 + 1 + 32:
            log.warning("RESP_CODE_SELF_INFO too short: %d bytes", len(payload))
            return

        i = 1  # skip code byte
        # adv_type = payload[i]; i+=1  (unused, always ADV_TYPE_CHAT)
        i += 3  # adv_type + tx_power + max_tx_power
        pub_key = payload[i:i + PUB_KEY_SIZE]
        i += PUB_KEY_SIZE

        # Skip lat/lon (8), multi_acks(1), advert_loc_policy(1), telemetry(1), manual_add(1) = 12
        i += 12
        # freq(4) + bw(4) + sf(1) + cr(1) = 10
        i += 10

        name = payload[i:].decode('utf-8', errors='replace').rstrip('\x00')
        pubkey_prefix = pub_key[:6].hex()

        user = User(
            pubkey_prefix=pubkey_prefix,
            pubkey_full=bytes(pub_key),
            name=name or "self",
            adv_type=ADV_TYPE_CHAT,
        )

        with self._state.lock():
            self._state.self_user = user
            self._state.conn_state = "connected"

        self._broadcast({
            "type": "conn",
            "state": "connected",
            "self": user.to_api(),
        })
        self._self_info_event.set()
        log.info("Connected as '%s' (%s)", name, pubkey_prefix)

    def _handle_contact(self, payload: bytes) -> None:
        """
        RESP_CODE_CONTACT layout (from writeContactRespFrame):
          [3, pub_key[32], type, flags, out_path_len, out_path[64], name[32],
           last_advert_ts[4], lat[4], lon[4], lastmod[4]]
        Minimum meaningful length: 1+32+1+1+1+64+32 = 132 bytes.
        """
        if len(payload) < 1 + PUB_KEY_SIZE + 3:
            return
        i = 1
        pub_key = bytes(payload[i:i + PUB_KEY_SIZE])
        i += PUB_KEY_SIZE
        adv_type = payload[i]
        i += 1
        # flags, out_path_len, out_path[64] — skip
        i += 1 + 1 + 64  # flags + out_path_len + out_path
        name_raw = payload[i:i + 32]
        name = name_raw.split(b'\x00')[0].decode('utf-8', errors='replace')

        # Remaining: last_advert_ts[4], lat[4], lon[4], lastmod[4]
        last_seen = None
        j = i + 32
        if len(payload) >= j + 4:
            (last_ts,) = struct.unpack_from('<I', payload, j)
            last_seen = last_ts if last_ts > 0 else None

        pubkey_prefix = pub_key[:6].hex()
        user = User(
            pubkey_prefix=pubkey_prefix,
            pubkey_full=pub_key,
            name=name,
            adv_type=adv_type,
            last_seen=last_seen,
        )
        with self._state.lock():
            self._state.users[pubkey_prefix] = user

    def _handle_end_of_contacts(self) -> None:
        with self._state.lock():
            self._state.rebuild_channels_from_users()

        self._broadcast({
            "type": "users",
            "users": [u.to_api() for u in self._state.users.values()],
        })
        self._broadcast({
            "type": "channels",
            "channels": [ch.to_api() for ch in self._state.channels.values()],
        })
        self._contacts_done_event.set()
        log.debug("Contacts sync done: %d contacts", len(self._state.users))

        # Request channel info for known group channels (idx 0..7)
        for idx in range(8):
            self.request_channel(idx)

    def _handle_channel_info(self, payload: bytes) -> None:
        """
        RESP_CODE_CHANNEL_INFO layout:
          [18, channel_idx, name[32], secret[16]]
        """
        if len(payload) < 1 + 1 + 32:
            return
        channel_idx = payload[1]
        name = payload[2:34].split(b'\x00')[0].decode('utf-8', errors='replace')
        if not name:
            return  # Empty slot — skip

        cid = f"chan:{channel_idx}"
        ch = Channel(
            id=cid,
            kind="channel",
            name=name,
            display_name=f"#{name}",
            locked=False,
            channel_idx=channel_idx,
        )
        with self._state.lock():
            self._state.channels[cid] = ch

        self._broadcast({
            "type": "channels",
            "channels": [c.to_api() for c in self._state.channels.values()],
        })
        log.debug("Channel idx=%d name='%s'", channel_idx, name)

    def _handle_resp_sent(self, payload: bytes) -> None:
        """
        RESP_CODE_SENT layout: [6, flood_flag, expected_ack[4], est_timeout[4]]
        """
        if len(payload) < 10:
            return
        (expected_ack,) = struct.unpack_from('<I', payload, 2)

        with self._state.lock():
            # Resolve the most-recent pending message (slot 0)
            pending = self._state.pending_msgs.pop(0, None)
            if expected_ack:
                # Store by expected_ack for later PUSH_CODE_SEND_CONFIRMED
                if pending:
                    self._state.pending_msgs[expected_ack] = pending

        if pending:
            msg_id, channel_id = pending
            # Emit status: sent
            self._broadcast({
                "type": "message",
                "message": {
                    "id": msg_id,
                    "channelId": channel_id,
                    "from": "",
                    "self": True,
                    "text": "",  # frontend already has the text
                    "ts": int(time.time()),
                    "status": "sent",
                },
            })

    def _handle_send_confirmed(self, payload: bytes) -> None:
        """
        PUSH_CODE_SEND_CONFIRMED: [0x82, ack_hash[4], trip_time[4]]
        """
        if len(payload) < 5:
            return
        (ack,) = struct.unpack_from('<I', payload, 1)

        with self._state.lock():
            pending = self._state.pending_msgs.pop(ack, None)

        if pending:
            msg_id, channel_id = pending
            self._broadcast({
                "type": "message",
                "message": {
                    "id": msg_id,
                    "channelId": channel_id,
                    "from": "",
                    "self": True,
                    "text": "",
                    "ts": int(time.time()),
                    "status": "confirmed",
                },
            })

    def _handle_contact_msg_v3(self, payload: bytes) -> None:
        """
        RESP_CODE_CONTACT_MSG_RECV_V3 layout:
          [16, snr_i8, reserved, reserved,
           pub_key_prefix[6], path_len, txt_type, ts_u32[4],
           (extra[4] if txt_type==SIGNED_PLAIN),
           text...]
        """
        if len(payload) < 4 + 6 + 1 + 1 + 4:
            return
        i = 1  # skip code
        # snr_i8 = struct.unpack_from('b', payload, i)[0]; i+=1
        i += 3  # snr + 2 reserved
        pubkey_prefix = payload[i:i + 6].hex()
        i += 6
        # path_len = payload[i]; i+=1  (unused)
        i += 1
        txt_type = payload[i]; i += 1
        if len(payload) < i + 4:
            return
        (ts,) = struct.unpack_from('<I', payload, i)
        i += 4

        # Skip extra bytes for signed messages (4 bytes sender prefix)
        TXT_TYPE_SIGNED_PLAIN = 2
        if txt_type == TXT_TYPE_SIGNED_PLAIN:
            i += 4

        text = payload[i:].decode('utf-8', errors='replace')

        # Map to channel
        user = self._state.users.get(pubkey_prefix)
        if user and user.adv_type == ADV_TYPE_ROOM:
            channel_id = f"room:{pubkey_prefix}"
        else:
            channel_id = f"dm:{pubkey_prefix}"

        sender_name = user.name if user else pubkey_prefix
        msg = Message(
            id=str(uuid.uuid4()),
            channel_id=channel_id,
            from_name=sender_name,
            self_msg=False,
            text=text,
            ts=ts if ts > 0 else int(time.time()),
            status="confirmed",
        )
        with self._state.lock():
            self._state.upsert_message(msg)

        self._broadcast({"type": "message", "message": msg.to_api()})

        # Keep draining the queue
        self._drain_messages()

    def _handle_channel_msg_v3(self, payload: bytes) -> None:
        """
        RESP_CODE_CHANNEL_MSG_RECV_V3 layout:
          [17, snr_i8, reserved, reserved,
           channel_idx, path_len, txt_type, ts_u32[4], text...]
        Text is formatted as "SenderName: actual message" by firmware.
        """
        if len(payload) < 4 + 1 + 1 + 1 + 4:
            return
        i = 1
        i += 3  # snr + 2 reserved
        channel_idx = payload[i]; i += 1
        i += 1  # path_len (unused)
        i += 1  # txt_type (always PLAIN for group)
        if len(payload) < i + 4:
            return
        (ts,) = struct.unpack_from('<I', payload, i)
        i += 4

        raw_text = payload[i:].decode('utf-8', errors='replace')

        # Parse "SenderName: message" prefix added by firmware
        sender_name = ""
        text = raw_text
        if ': ' in raw_text:
            parts = raw_text.split(': ', 1)
            sender_name = parts[0]
            text = parts[1]

        channel_id = f"chan:{channel_idx}"

        msg = Message(
            id=str(uuid.uuid4()),
            channel_id=channel_id,
            from_name=sender_name,
            self_msg=False,
            text=text,
            ts=ts if ts > 0 else int(time.time()),
            status="confirmed",
        )
        with self._state.lock():
            self._state.upsert_message(msg)

        self._broadcast({"type": "message", "message": msg.to_api()})

        # Keep draining
        self._drain_messages()

    def _handle_login_result(self, payload: bytes, success: bool) -> None:
        """
        PUSH_CODE_LOGIN_SUCCESS: [0x85, permissions, pub_key_prefix[6], ...]
        PUSH_CODE_LOGIN_FAIL:    [0x86, reserved,    pub_key_prefix[6]]
        """
        if len(payload) < 1 + 1 + 6:
            return
        pubkey_prefix = payload[2:8].hex()
        channel_id = f"room:{pubkey_prefix}"

        if success:
            with self._state.lock():
                ch = self._state.channels.get(channel_id)
                if ch:
                    ch.locked = False
                    ch.joined = True

        self._broadcast({
            "type": "login",
            "channelId": channel_id,
            "ok": success,
            "error": None if success else "Login failed",
        })
        self._broadcast({
            "type": "channels",
            "channels": [c.to_api() for c in self._state.channels.values()],
        })


# ---------------------------------------------------------------------------
# Module-level singleton

_bridge: Optional[SerialBridge] = None


def get_bridge() -> Optional[SerialBridge]:
    return _bridge


def init_bridge(state: AppState, broadcast_fn: Callable[[dict], None]) -> SerialBridge:
    global _bridge
    _bridge = SerialBridge(state, broadcast_fn)
    return _bridge
