# SIREN Web Client — API Contract (v1, FROZEN)

Owner: DevArchitect. Do not change without updating this file first and pinging both
FrontendDeveloper and BackendDeveloper. This contract lets frontend + backend build in
parallel.

Base URL: `http://127.0.0.1:8760` (port configurable). All bodies are JSON, UTF-8.

## Data shapes

```ts
type ConnState = "disconnected" | "connecting" | "connected" | "error";

interface SerialPort {
  path: string;        // e.g. "COM5" or "/dev/ttyUSB0"
  label: string;       // human description
}

// A channel is any addressable chat target: room, group channel, or DM.
interface Channel {
  id: string;          // stable id: "room:<pubkeyPrefixHex>" | "chan:<idx>" | "dm:<pubkeyPrefixHex>"
  kind: "room" | "channel" | "dm";
  name: string;        // display name, e.g. "siren-ops"
  displayName: string; // with prefix, e.g. "#siren-ops", "#general", "alice"
  locked: boolean;     // room requiring login and not yet authenticated
  joined: boolean;     // currently open/subscribed in the UI session
  unread: number;
}

interface User {
  pubkeyPrefix: string; // hex, 6 bytes = 12 hex chars
  name: string;
  isRoom: boolean;
  lastSeen: number | null; // epoch seconds
}

interface Message {
  id: string;          // client-unique
  channelId: string;
  from: string;        // sender name or pubkey prefix; "" for self
  self: boolean;
  text: string;
  ts: number;          // epoch seconds
  status: "pending" | "sent" | "confirmed" | "failed"; // outbound lifecycle; inbound = "confirmed"
}
```

## REST endpoints

| Method | Path | Body | Response |
|--------|------|------|----------|
| GET  | `/api/ports` | — | `{ ports: SerialPort[] }` (enumerated, allow-list) |
| POST | `/api/connect` | `{ path: string, baud?: number }` | `{ state: ConnState, self?: User }` |
| POST | `/api/disconnect` | — | `{ state: ConnState }` |
| GET  | `/api/state` | — | `{ conn: ConnState, self: User|null, channels: Channel[], users: User[] }` |
| GET  | `/api/channels/:id/messages?limit=100` | — | `{ messages: Message[] }` (RAM buffer) |
| POST | `/api/channels/:id/join` | `{ password?: string }` | `{ ok: boolean, channel: Channel, error?: string }` |
| POST | `/api/channels/:id/part` | — | `{ ok: boolean }` |
| POST | `/api/channels/:id/messages` | `{ text: string }` | `{ ok: boolean, message: Message }` |
| POST | `/api/advert` | `{ name?: string, flood?: boolean }` | `{ ok: boolean }` |

Rules:
- `path` for `/api/connect` MUST match an entry returned by `/api/ports` (reject otherwise → 400).
- `text` is trimmed and length-clamped to the firmware limit; empty → 400.
- `join` on a `kind:"room"` with a password triggers `CMD_SEND_LOGIN`; result also arrives
  async via WS `login` event.
- All 4xx errors return `{ error: string }`.

## WebSocket `/ws`

Server→client events (each is `{ type, ...}` JSON):

| type | payload |
|------|---------|
| `conn` | `{ state: ConnState, self?: User, error?: string }` |
| `channels` | `{ channels: Channel[] }` (full snapshot on change) |
| `users` | `{ users: User[] }` |
| `message` | `{ message: Message }` (inbound or status update; frontend upserts by `message.id`) |
| `login` | `{ channelId: string, ok: boolean, error?: string }` |
| `notice` | `{ level: "info"|"warn"|"error", text: string }` |

Client→server: none required for v1 (all actions go via REST). WS is push-only.
Backend MUST reject WS upgrades with a non-local `Origin`.

## Outbound message lifecycle

1. `POST /api/channels/:id/messages` → returns `Message{status:"pending"}` immediately and
   the frontend renders it optimistically.
2. Backend frames it to serial; on `RESP_CODE_SENT` emits WS `message` with same `id`,
   `status:"sent"`.
3. On `PUSH_CODE_SEND_CONFIRMED` → WS `message` `status:"confirmed"`. On timeout/err →
   `status:"failed"`.

Frontend upserts messages by `id`, so status transitions update the existing bubble.
