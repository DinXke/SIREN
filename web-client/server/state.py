"""
In-memory application state for the SIREN web client bridge.

All state is ephemeral — no database. Single user, local use only.
"""

import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Optional

# Firmware limit: MAX_TEXT_LEN = 10 * CIPHER_BLOCK_SIZE (16) = 160
MAX_TEXT_LEN = 160

# Keep last N messages per channel in RAM
MAX_MSG_HISTORY = 200

# MeshCore ADV_TYPE constants (from firmware/src/helpers/AdvertDataHelpers.h)
ADV_TYPE_NONE = 0
ADV_TYPE_CHAT = 1
ADV_TYPE_REPEATER = 2
ADV_TYPE_ROOM = 3
ADV_TYPE_SENSOR = 4


@dataclass
class User:
    pubkey_prefix: str   # 12 hex chars (6 bytes)
    pubkey_full: bytes   # 32 bytes
    name: str
    adv_type: int        # ADV_TYPE_* constant
    last_seen: Optional[int] = None  # epoch seconds

    @property
    def is_room(self) -> bool:
        return self.adv_type == ADV_TYPE_ROOM

    def to_api(self) -> dict:
        return {
            "pubkeyPrefix": self.pubkey_prefix,
            "name": self.name,
            "isRoom": self.is_room,
            "lastSeen": self.last_seen,
        }


@dataclass
class Channel:
    id: str           # "room:<prefix>" | "chan:<idx>" | "dm:<prefix>"
    kind: str         # "room" | "channel" | "dm"
    name: str
    display_name: str
    locked: bool
    joined: bool = False
    unread: int = 0
    # Internal routing info
    channel_idx: Optional[int] = None   # for kind=="channel"
    pubkey_prefix: Optional[str] = None  # for kind=="room" or "dm"

    def to_api(self) -> dict:
        return {
            "id": self.id,
            "kind": self.kind,
            "name": self.name,
            "displayName": self.display_name,
            "locked": self.locked,
            "joined": self.joined,
            "unread": self.unread,
        }


@dataclass
class Message:
    id: str
    channel_id: str
    from_name: str    # sender display name; "" for self
    self_msg: bool
    text: str
    ts: int           # epoch seconds
    status: str       # "pending"|"sent"|"confirmed"|"failed"

    def to_api(self) -> dict:
        return {
            "id": self.id,
            "channelId": self.channel_id,
            "from": self.from_name,
            "self": self.self_msg,
            "text": self.text,
            "ts": self.ts,
            "status": self.status,
        }


class AppState:
    """
    Thread-safe in-memory state. A single global instance is shared between
    the Flask request handlers and the serial bridge background thread.
    """

    def __init__(self) -> None:
        self._lock = threading.RLock()

        # Connection
        self.conn_state: str = "disconnected"  # ConnState
        self.self_user: Optional[User] = None

        # Contacts indexed by 12-char pubkey_prefix hex
        self.users: dict[str, User] = {}

        # Channels indexed by channel id string
        self.channels: dict[str, Channel] = {}

        # Messages: channel_id -> deque of Message
        self.messages: dict[str, deque] = {}

        # Pending outbound messages: expected_ack_int -> (msg_id, channel_id)
        self.pending_msgs: dict[int, tuple[str, str]] = {}

    # ------------------------------------------------------------------
    # Locking helpers

    def lock(self):
        return self._lock

    # ------------------------------------------------------------------
    # Channels helpers

    def get_or_create_messages(self, channel_id: str) -> deque:
        if channel_id not in self.messages:
            self.messages[channel_id] = deque(maxlen=MAX_MSG_HISTORY)
        return self.messages[channel_id]

    def upsert_message(self, msg: Message) -> None:
        """Insert or update a message in the channel history."""
        q = self.get_or_create_messages(msg.channel_id)
        # Check if message with same id already exists (status update)
        for i, existing in enumerate(q):
            if existing.id == msg.id:
                q[i] = msg
                return
        q.append(msg)
        # Bump unread counter for joined channels
        ch = self.channels.get(msg.channel_id)
        if ch and ch.joined and not msg.self_msg:
            with self._lock:
                ch.unread += 1

    def snapshot(self) -> dict:
        """Return a JSON-serializable snapshot of the full state."""
        with self._lock:
            return {
                "conn": self.conn_state,
                "self": self.self_user.to_api() if self.self_user else None,
                "channels": [ch.to_api() for ch in self.channels.values()],
                "users": [u.to_api() for u in self.users.values()],
            }

    def rebuild_channels_from_users(self) -> None:
        """
        Build channel entries for ADV_TYPE_ROOM and ADV_TYPE_CHAT contacts.
        Called after a contacts sync completes.
        Group channels (chan:*) are populated separately via CMD_GET_CHANNEL.
        """
        with self._lock:
            for prefix, user in self.users.items():
                if user.adv_type == ADV_TYPE_ROOM:
                    cid = f"room:{prefix}"
                    if cid not in self.channels:
                        self.channels[cid] = Channel(
                            id=cid,
                            kind="room",
                            name=user.name,
                            display_name=f"#siren-{user.name}",
                            locked=True,
                            pubkey_prefix=prefix,
                        )
                elif user.adv_type == ADV_TYPE_CHAT:
                    cid = f"dm:{prefix}"
                    if cid not in self.channels:
                        self.channels[cid] = Channel(
                            id=cid,
                            kind="dm",
                            name=user.name,
                            display_name=user.name,
                            locked=False,
                            pubkey_prefix=prefix,
                        )


# Module-level singleton
_state = AppState()


def get_state() -> AppState:
    return _state
