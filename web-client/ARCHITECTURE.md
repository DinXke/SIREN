# SIREN Web Client — Architecture

Status: **Approved for build** (DevArchitect, JES-731)
Branch: `multiroom`

## 1. Problem / requirement

JES-729/JES-731 request an IRC-style ("mIRC") web UI for SIREN (MeshCore-based LoRa
mesh). Sidebar of channels, main chat pane, user list, input bar. Multiple rooms open
at once. Connect over **serial** (primary) to a SIREN Heltec LoRa32 V3; Bluetooth is a
phase-2 nice-to-have. Local use only, no authentication at this stage.

## 2. Key architectural decision — what the browser connects to

A MeshCore **room server has no client chat protocol** — over serial it only exposes an
admin CLI (`room list`, `room add`, ...). To *participate* in chat as an IRC client, the
web app must talk to a **companion_radio node** (MeshCore's client-node firmware), exactly
like the official MeshCore phone app does. The companion node is itself a SIREN Heltec
LoRa32 V3 flashed with `examples/companion_radio`; it relays over LoRa to the room
servers and other contacts on the mesh.

```
Browser (React IRC UI)
   |  HTTP REST + WebSocket (localhost)
Flask bridge (Python + pyserial)
   |  MeshCore companion serial frame protocol (USB CDC, 115200)
SIREN companion_radio node (Heltec LoRa32 V3)
   )))  LoRa mesh  (((
Room servers (#siren- channels) · group channels (#channels) · contacts (DMs)
```

This is the only technically-correct topology for an IRC *client*. If the intent was
instead to wrap the room-server admin CLI, that is a different tool — flagged to CTO in
the JES-731 thread; build proceeds on the companion-node interpretation.

## 3. IRC analogy → MeshCore mapping

| IRC concept        | MeshCore object                                              | Visual |
|--------------------|--------------------------------------------------------------|--------|
| `#siren-<name>`    | Room-server contact (`ADV_TYPE_ROOM`), login-gated           | badge / lock prefix |
| `#<name>`          | Group channel (`CMD_GET_CHANNEL` / `CMD_SEND_CHANNEL_TXT_MSG`)| `#` prefix |
| DM / query         | Regular contact (`ADV_TYPE_CHAT`), `CMD_SEND_TXT_MSG`        | user icon |
| user list          | Known contacts on the mesh (`CMD_GET_CONTACTS`)              | right pane |
| join / part        | open/close a channel or room tab; room join = `CMD_SEND_LOGIN` | — |

## 4. Stack (decision)

- **Backend:** Python 3.11+, **Flask** + **Flask-Sock** (WebSocket), **pyserial**. Single
  process. Boring, stable, matches proposal.
- **Frontend:** **React + Vite + TypeScript**, single page. Built to static assets served
  by Flask so the whole thing runs as one app on one port.
- **State:** in-memory in the Flask process (contacts, channels, message buffers). No DB —
  local, ephemeral, single user. Message history is best-effort in RAM.
- **Serial protocol:** MeshCore companion frame protocol. Command/response/push constants
  are defined in `firmware/examples/companion_radio/MyMesh.cpp` (top of file) — treat that
  file as the authoritative protocol reference.

Alternatives rejected: FastAPI (async nice but Flask is simpler and specified); Electron
(too heavy for a local tool); direct Web Serial API in the browser (no pyserial-grade
control, no BLE fallback path, poorer cross-platform behaviour).

## 5. Serial frame protocol (what BackendDeveloper implements)

Transport framing over USB serial (MeshCore `SerialInterface`):
- Host→device frame: `'>'` + `uint16` little-endian length + payload.
- Device→host frame: `'<'` + `uint16` little-endian length + payload.
- Payload byte 0 = command / response / push code.

Minimum command set for v1 (codes from `companion_radio/MyMesh.cpp`):

| Purpose            | Code                                   | Payload after code |
|--------------------|----------------------------------------|--------------------|
| App start / handshake | `CMD_APP_START` (1)                 | app ver + name |
| Get contacts (sync)   | `CMD_GET_CONTACTS` (4)              | optional `since` u32 |
| Get channel info      | `CMD_GET_CHANNEL` (31)             | channel_idx |
| Send DM               | `CMD_SEND_TXT_MSG` (2)             | txt_type, attempt, timestamp u32, pubkey_prefix[6], text |
| Send channel msg      | `CMD_SEND_CHANNEL_TXT_MSG` (3)     | txt_type=PLAIN, channel_idx, timestamp u32, text |
| Room login            | `CMD_SEND_LOGIN` (26)              | pubkey_prefix[6], password |
| Sync next message     | `CMD_SYNC_NEXT_MESSAGE` (10)       | — |
| Self advert           | `CMD_SEND_SELF_ADVERT` (7)         | flags |
| Set advert name       | `CMD_SET_ADVERT_NAME` (8)          | name |

Key responses / pushes to handle:
- `RESP_CODE_SELF_INFO` (5), `RESP_CODE_CONTACTS_START/CONTACT/END_OF_CONTACTS` (2/3/4),
  `RESP_CODE_SENT` (6), `RESP_CODE_CHANNEL_INFO` (18),
  `RESP_CODE_CONTACT_MSG_RECV_V3` (16), `RESP_CODE_CHANNEL_MSG_RECV_V3` (17),
  `RESP_CODE_NO_MORE_MESSAGES` (10).
- Pushes: `PUSH_CODE_MSG_WAITING` (0x83) → drain via `CMD_SYNC_NEXT_MESSAGE` loop;
  `PUSH_CODE_ADVERT`/`NEW_ADVERT` (0x80/0x8A) → refresh contacts;
  `PUSH_CODE_LOGIN_SUCCESS`/`LOGIN_FAIL` (0x85/0x86) → room join result;
  `PUSH_CODE_SEND_CONFIRMED` (0x82) → delivery ack.

Backend responsibility: own the serial read loop on a background thread, decode frames
into normalized JSON events, keep contact/channel/message state, and push events to the
browser over WebSocket. The exact wire byte layout beyond byte 0 should be read directly
from `companion_radio/MyMesh.cpp` (`queueMessage`, the `CMD_*` handlers ~line 1071+, and
the `RESP_CODE_*`/`PUSH_CODE_*` emitters).

## 6. Security implications (local tool, but still)

- **No network exposure by default.** Bind Flask to `127.0.0.1` only. Port configurable,
  but never default to `0.0.0.0`.
- **No auth by design** (per requirement) — acceptable *only* because it is loopback-bound.
  README must state that binding to a non-loopback address exposes the mesh with no auth.
- **Input validation at the boundary:** validate/clamp text length to the firmware limit
  (`MAX_POST_TEXT_LEN`), reject non-UTF-8/oversized WS payloads, validate `channel_idx` /
  `pubkey_prefix` shape before framing. Never forward raw browser bytes to the serial port.
- **Room passwords** travel browser→backend→serial in cleartext (LoRa/companion design).
  Do not log them; keep them out of message history and out of any debug dump.
- **Serial device path** comes from the user via an allow-list of enumerated ports, not a
  free-form path the browser can set to an arbitrary file (path-injection guard).
- **WebSocket origin check:** reject WS upgrades whose `Origin` is not the local app.

## 7. Deployment

- Single Flask process serves the built React bundle + REST + WS on one configurable port
  (default `127.0.0.1:8760`).
- `install.sh` (Linux/macOS) and `install.bat` (Windows): create venv, `pip install`,
  `npm ci && npm run build` into `web-client/server/static/`, then start the server and
  print the URL.
- Layout:
  ```
  web-client/
    server/        # Flask app, serial bridge, requirements.txt
    frontend/      # React + Vite + TS
    install.sh
    install.bat
    README.md
  ```

## 8. Delegation

- **BackendDeveloper** → Flask serial bridge + REST/WS API + install scripts. (child issue)
- **FrontendDeveloper** → React IRC SPA against the API contract. (child issue)
- API contract is frozen in `API_CONTRACT.md` before both start, so they work in parallel.
