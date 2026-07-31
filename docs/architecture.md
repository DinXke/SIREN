# Multiroom Architecture

This document explains how SIREN runs multiple independent chat rooms on a single device. It is intended for developers and advanced operators who want to understand the internals.

---

## Overview

A standard MeshCore room server hosts exactly one room (one keypair, one name, one member list). SIREN extends this with a **multiroom** model: a single Heltec device hosts up to **16 virtual room servers** simultaneously, each behaving as an independent room server to the outside world.

From the perspective of a MeshCore companion radio user, each SIREN room looks like a completely separate room server node on the mesh — it has its own cryptographic identity (Ed25519 keypair), its own name, its own member list (ACL), and its own post history.

---

## Key Data Structures

### RoomSlot

Each virtual room is stored in a `RoomSlot` structure:

```cpp
struct RoomSlot {
  bool              active;           // is this slot in use?
  mesh::LocalIdentity id;             // Ed25519 keypair for this room
  char  name[24];                     // advertised room name
  char  password[16];                 // admin/login password
  char  guest_password[16];           // guest password (empty = no password)
  float lat, lon;                     // GPS coordinates (optional)
  ClientACL  acl;                     // member access control list
  bool  stealth;                      // true = no adverts (default)
  // ... timing fields for advert scheduling ...
};
```

Up to `MAX_ROOMS = 16` slots exist in RAM. Room slot 0 always exists (it is created on first boot if no config is found). Additional rooms are created and deleted at runtime via CLI or Web UI.

### Global Post Pool

Messages (posts) are **not** stored per-room. Instead, all posts across all rooms share a single global pool:

```cpp
PostInfo _post_pool[MAX_TOTAL_POSTS];  // MAX_TOTAL_POSTS = 128
```

Each `PostInfo` entry knows which room it belongs to (`room_idx` field). When a room is pushed a new post, the firmware looks for a free slot in the pool. If the pool is full, the oldest post from that room is evicted (FIFO within each room's quota).

**Per-room quota**: The effective quota per room scales with how many rooms are active. With 2 active rooms: up to 64 posts/room. With 16 active rooms: 8 posts/room.

**Important**: Posts are stored in RAM only. A power cycle or reboot clears all messages. Room configuration and identities are persisted to SPIFFS and survive reboots.

### PeerInfo (Replication Groundwork)

Up to `MAX_PEERS = 8` remote room-server nodes can be registered for future anti-entropy replication (Phase 5 and beyond). Each peer stores:

```cpp
struct PeerInfo {
  bool    active;
  char    name[24];
  uint8_t pub_key[32];      // Ed25519 public key of the peer node
  uint32_t last_contact;    // RTC timestamp of last heard packet
};
```

Peer configuration is stored in SPIFFS (`/peer_cfg`) and survives reboots.

---

## Cryptographic Identity Per Room

Each room has its **own** Ed25519 keypair. This means:

- Each room has a unique public key → unique "address" on the mesh
- Incoming packets are matched to rooms by destination hash (truncated public key)
- Signing and encryption uses the room's private key, not a shared device key

The firmware manages this by temporarily swapping the `self_id` field in the Mesh base class to the target room's identity before sending a packet, then restoring it. This works safely because the firmware is single-threaded (one event loop, no concurrency).

Room identities are stored in SPIFFS under keys like `_room0`, `_room1`, etc.

---

## Packet Dispatch: How Does a Packet Find Its Room?

When a LoRa packet arrives, MeshCore calls `onRecvPacket()`. The SIREN override searches all active room slots for a hash match:

```
Incoming packet (dest hash = first N bytes of room's public key)
    │
    ├── try room[0]: hash matches? → try decrypt → success → handle as room[0]
    ├── try room[1]: hash matches? → try decrypt → success → handle as room[1]
    ├── ...
    └── no match → drop or forward
```

If two rooms happen to share the same hash prefix (a collision), the firmware tries all matching rooms and uses the one where decryption succeeds. This is the JES-732 dest-hash collision fix.

---

## Room 0: The Management Room

Room 0 is special:

- It always exists (cannot be deleted)
- It is the room whose ACL is exposed to the `CommonCLI` (`setperm`, `get acl` commands)
- It is used as the base identity for the Mesh (`self_id = rooms[0].id` at startup)
- Repeater configuration commands are restricted to authenticated admins of room 0 (the "management room")

All other rooms (1-15) can be created, renamed, and deleted freely.

---

## Stealth Mode (Default)

All rooms default to **stealth mode** — they do NOT send advertisements on the LoRa mesh. This means a device listening on the frequency will not discover the room unless:

1. The operator explicitly turns stealth off (`room stealth <idx> off`)
2. The operator shares a **join URI** or **QR code** out-of-band (e.g., scanned from the web UI)

This is intentional for security: SIREN rooms are opt-in for discovery. Distributing join URIs/QR codes is the primary out-of-band join path.

When stealth is off, rooms advertise every 2 minutes (local/zero-hop) and every 47 hours (flood).

---

## ACL and Permissions

Each room maintains its own `ClientACL` — a list of known members and their permission levels:

| Permission | Who can | Value |
|---|---|---|
| `PERM_ACL_ADMIN` | Admin: read, write, manage members | highest |
| `PERM_ACL_READ_WRITE` | Regular member: read and post | medium |
| `PERM_ACL_READ_ONLY` | Observer: read only | low |
| `PERM_ACL_GUEST` | Guest: limited access, telemetry blocked | lowest |

A client is added to the ACL when they successfully log in (provide the correct room password on first contact). Admins can then adjust permissions via `room setperm`.

---

## Post Delivery Flow

When a member sends a post to a room:

```
1. Member sends encrypted DM to room's public key
2. Room server receives, decrypts, authenticates sender (must be in ACL with write permission)
3. Post stored in global _post_pool[]
4. Server round-robins through member list, pushing undelivered posts to each member
   - Push: encrypted direct message to member's public key
   - Member sends ACK when received
   - If ACK not received within timeout, retry up to 3 times
   - After 3 failures, member is considered offline; push attempts stop
```

The server does not push posts back to the original author (they already have their own copy).

---

## Memory Budget

At compile time, with `MAX_ROOMS=16` and `MAX_TOTAL_POSTS=128`:

```
RoomSlot[16]        ≈ 16 * ~200 bytes    = ~3.2 KB
PostInfo[128]       ≈ 128 * ~180 bytes   = ~23 KB
PeerInfo[8]         ≈ 8 * ~60 bytes      = ~0.5 KB
Packet pool (32)    ≈ 32 * ~256 bytes    = ~8 KB
Total data overhead ≈ ~35 KB
```

ESP32-S3 has 512 KB RAM. At v1.16 baseline + Phase 1-3 implementation, measured RAM usage is approximately **40.5%** (~207 KB), leaving healthy headroom.

---

## SPIFFS File Layout

All persistent data is stored in SPIFFS (the on-chip filesystem):

| Path | Contents |
|---|---|
| `/identity/_room0` | Ed25519 keypair for room 0 |
| `/identity/_room1` | Ed25519 keypair for room 1 |
| ... | |
| `/room_cfg` | Binary blob: active flags, names, passwords for all rooms |
| `/peer_cfg` | Binary blob: peer room server list |
| `/wifi_sta.json` | WiFi mode and credentials |
| (NVS via CommonCLI) | Radio settings (freq/BW/SF/CR/TX), node name, admin password, flood params |

---

## Sequence Diagram: User Joins a Room

```
User App              Companion Radio        SIREN Room Server
    │                       │                       │
    │  "join room"          │                       │
    │──────────────────────>│                       │
    │                       │  login request        │
    │                       │──────────────────────>│
    │                       │              auth: check password
    │                       │              add to ACL if new
    │                       │  login OK (RESP=0)    │
    │                       │<──────────────────────│
    │  connected            │                       │
    │<──────────────────────│                       │
    │                       │                       │
    │  "post message"       │                       │
    │──────────────────────>│                       │
    │                       │  encrypted DM         │
    │                       │──────────────────────>│
    │                       │              store in post pool
    │                       │              push to other members
    │                       │  ACK                  │
    │                       │<──────────────────────│
    │  delivered            │                       │
    │<──────────────────────│                       │
```

---

## Build Flags

Key build-time constants (set in `firmware/variants/heltec_v3/platformio.ini`):

| Flag | Default | Meaning |
|---|---|---|
| `MAX_ROOMS` | 16 | Maximum virtual rooms per device |
| `MAX_TOTAL_POSTS` | 128 | Global post pool size (shared across all rooms) |
| `MAX_PEERS` | 8 | Maximum peer room servers for replication |
| `ADMIN_PASSWORD` | `password` | Default admin password (change post-flash) |
| `LORA_FREQ` | 869.618 | Radio frequency in MHz |
| `LORA_BW` | 62.5 | Bandwidth in kHz |
| `LORA_SF` | 8 | Spreading factor |
| `LORA_CR` | 8 | Coding rate denominator (4/8) |
| `LORA_TX_POWER` | 20 | TX power in dBm (SIREN env sets to 22 via platformio.ini) |

---

## Source Code Location

The SIREN room server implementation lives entirely in:

```
firmware/examples/siren_room_server/
    MyMesh.h      — class declaration, structs, build-flag defaults
    MyMesh.cpp    — all implementation (~1296 lines)
    main.cpp      — Arduino setup()/loop(), CLI dispatch
    WebManager.h  — WiFi AP/STA + web server class
    WebManager.cpp — web routes, REST API, backup/restore
    UITask.h      — OLED display task
    UITask.cpp    — display rendering, screensaver
    SettingsMenu.h — interactive serial settings menu
```

The MeshCore base library lives in `firmware/src/` and `firmware/helpers/` and is **not modified by SIREN**. SIREN only extends classes from the base library.
