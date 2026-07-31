# CLI Reference

SIREN provides a command-line interface (CLI) via two channels:

1. **Serial CLI** — type commands into the USB serial console (115200 baud, CR line ending)
2. **CLI-over-mesh** — send commands as encrypted direct messages from another MeshCore node (admin permission required)

Both channels use the same command syntax. Responses are printed to serial (if available) or returned in the DM reply.

---

## How to Use the Serial CLI

Connect a USB cable, open a serial terminal at **115200 baud**, and type commands followed by **Enter** (CR/`\r`).

The firmware also provides an interactive **settings menu**:
```
> menu
```
This opens a full-screen interactive settings editor (navigate with arrow keys, exit with Esc).

---

## Room Management Commands

Room commands manage the virtual chat rooms hosted on this device.

### `room list` / `room ls`

List all room slots with their status.

```
> room list
Rooms (2/16 active):
  [0] ON  name='Triage'    stealth=on  id=A1B2C3D4...  clients=3  posts=12
  [1] ON  name='Logistics' stealth=off id=E5F6A7B8...  clients=1  posts=4
  [2] OFF name=''
  ...
```

### `room add`

Activate the next free room slot. A new Ed25519 keypair is generated (or loaded if one was previously saved for this slot index). The room is named `Room<N>` by default.

```
> room add
  -> OK - room[1] added, id=E5F6A7B8
```

### `room del <idx>`

Deactivate room slot `<idx>`. The room is removed from the active list and stops accepting connections. Its keypair remains saved in SPIFFS and can be reactivated. Room 0 cannot be deleted.

```
> room del 2
  -> OK
```

### `room set <idx> name <value>`

Change the advertised name of room `<idx>`. Maximum 23 characters.

```
> room set 0 name Triage
  -> OK
```

### `room set <idx> pass <value>`

Change the admin/login password for room `<idx>`.

```
> room set 0 pass secretpass
  -> OK
```

### `room set <idx> guest <value>`

Change the guest password for room `<idx>`. An empty guest password means the room requires the main password. Setting a non-empty guest password allows users to log in with reduced permissions using this guest password.

```
> room set 1 guest guestpass
  -> OK
```

### `room stealth <idx> on|off`

Enable or disable stealth mode for room `<idx>`.

- **on** (default): Room sends no advertisements on the LoRa mesh. Users must receive the join URI or QR code out-of-band.
- **off**: Room advertises itself every 2 minutes (local) and every 47 hours (flood).

```
> room stealth 0 off
  -> OK - room[0] stealth OFF (visible)

> room stealth 0 on
  -> OK - room[0] stealth ON
```

### `room qr <idx>`

Print the join URI for room `<idx>` to the serial console. The URI is in MeshCore companion app import format:

```
> room qr 0
Room[0] join URI:
meshcore://contact/add?name=Triage&public_key=a1b2c3...64hexchars...&type=3
```

The Web UI also shows this as a scannable QR code. Share the URI or QR code with users to let them join a stealth room.

### `room clients <idx>`

List all known clients (members) in room `<idx>` with their permission level and last-seen timestamp.

```
> room clients 0
room[0] 'Triage' — 2 client(s):
  [0] admin  A1B2C3D4E5F6...  last=1722345600
  [1] rw     B2C3D4E5F6A7...  last=1722340000
```

### `room setperm <idx> <hex_pubkey> <perms>`

Set the ACL permission for a specific client (identified by their public key prefix) in room `<idx>`.

Permission values:
- `0` — remove / ban
- `1` — guest (read-only, no telemetry)
- `2` — read-only
- `3` — read-write (normal member)
- `4` — admin

```
> room setperm 0 A1B2C3D4E5F6 3
  -> OK - room[0] perm set
```

The `<hex_pubkey>` can be a prefix (minimum 6 hex characters = 3 bytes) of the client's public key, as shown by `room clients`.

### `room status <idx>`

Show per-client sync status for room `<idx>` — how many undelivered posts are queued for each member.

```
> room status 0
room[0] 'Triage'  posts=12  clients=2:
  [0] admin  A1B2C3D4E5F6...  last_act=1722345600  unsynced=0
  [1] rw     B2C3D4E5F6A7...  last_act=1722340000  unsynced=3
```

---

## Peer Management Commands

Peer commands configure remote room-server nodes for future anti-entropy replication (Phase 5).

### `peer list`

List all configured peer nodes.

```
> peer list
```

### `peer add <hex64_pubkey> <name>`

Add a peer room server by its 64-character hex public key and a human-readable name.

```
> peer add a1b2c3...64chars...  Node2
```

### `peer del <idx>`

Remove a peer entry.

```
> peer del 0
```

### `peer status`

Show last-contact timestamps for all peers.

### `peer sync`

Manually trigger a synchronisation attempt with all configured peers (Phase 5 feature, requires replication to be implemented).

---

## IRC / Chat Commands

These commands let operators inspect room messages and the user list, and post messages as the server operator. They work over both **serial CLI** and **mesh CLI** (Phase 4 admin DM).

### `rooms`

List all active rooms with their index, name, connected client count, post count, and stealth status.

```
rooms
```

**Example output:**
```
Active rooms (2):
  [0] SIREN                   clients=3  posts=12  stealth=off
  [1] ICT-Extern              clients=1  posts=4   stealth=on
```

---

### `msgs <room-idx> [n]`

Show the last `n` posts from the specified room (default: 10, max: 50). Author names are resolved from the advert name table; unknown nodes fall back to an 8-character hex prefix.

```
msgs 0
msgs 0 20
```

**Example output (serial):**
```
Room [0] 'SIREN' — 10/12 posts shown:
  [1753000001] <Alice> Afdeling 2 bevrijd, kom naar post Noord
  [1753000045] <[OP]> Noodprotocol geactiveerd
```

---

### `nicks <room-idx>`

Show the connected user list (nicklist) for the specified room, including each user's resolved name, role, and last-activity timestamp.

```
nicks 0
```

Roles: `guest` (0), `ro` read-only (1), `rw` read-write (2), `admin` (3).

---

### `say <room-idx> <text>`

Post a server-authored message to the specified room. The message is prefixed with `[OP]` to identify it as an operator post, and is pushed to all connected companions.

```
say 0 Noodprotocol geactiveerd — volg instructies van teamleider
```

**Notes:**
- Text is clamped to `MAX_POST_TEXT_LEN` (151 characters).
- Operator posts are stored in the global post pool and survive reboots.
- The operator identity is the room's own key pair, not a companion.

---

## WiFi Commands

WiFi commands are available when the `ENABLE_WIFI_MGMT` build flag is set (default in SIREN builds).

### `wifi mode ap`

Switch to AP (hotspot) mode.

### `wifi mode sta`

Switch to STA (client) mode.

### `wifi ap ssid <name>`

Set the AP hotspot SSID.

### `wifi ap pass <password>`

Set the AP hotspot password (empty = open).

### `wifi ssid <name>`

Set the STA WiFi network SSID to connect to.

### `wifi pass <password>`

Set the STA WiFi network password.

### `wifi connect`

Trigger a reconnection attempt with the current STA credentials.

### `wifi status`

Print current WiFi state, mode, SSID, and IP address.

---

## Standard MeshCore CLI Commands

The following commands are inherited from the MeshCore `CommonCLI` layer and work on all MeshCore nodes, including SIREN:

### Node identity and settings

| Command | Description |
|---|---|
| `set name <value>` | Set the node name (shown in adverts, OLED) |
| `set password <value>` | Set the admin password |
| `set freq <MHz>` | Set LoRa frequency |
| `set bw <kHz>` | Set LoRa bandwidth |
| `set sf <7-12>` | Set spreading factor |
| `set cr <5-8>` | Set coding rate (denominator; 4/5 to 4/8) |
| `set tx_power <dBm>` | Set transmit power |
| `radio` | Show current radio settings |
| `stats` | Show packet statistics |
| `radio stats` | Show radio-level statistics |
| `reboot` | Reboot the device |
| `reset` | Factory reset (erases SPIFFS — deletes keys, rooms, WiFi config) |

### Logging

| Command | Description |
|---|---|
| `log on` | Enable packet logging to serial |
| `log off` | Disable packet logging |

---

## CLI-over-Mesh

When USB serial is not available (e.g., on the board's glued unit), you can send commands from another MeshCore node with admin rights.

### How it works

1. On a second node with serial access, authenticate as admin to the room you want to manage.
2. Use your MeshCore companion app's "CLI DM" feature or the SIREN web client's command mode to send the command text.
3. The SIREN room server receives it as a specially-typed direct message (`TXT_TYPE_CLI_DATA`), executes the command, and returns the reply in a response DM.

### Limitations of CLI-over-mesh

- The reply is truncated to the DM payload limit (~160 characters). Commands that produce long output (like `room list` which prints to serial) will only return a short summary in the reply.
- `room list` and `room clients` output is verbose when sent to serial; over-mesh you get a single-line summary.
- `room qr` over mesh returns the URI in the reply (max 159 chars).

### Access control

CLI-over-mesh commands require the sender to be authenticated as **admin** in the target room. Non-admins receive an error or no response.

---

## Interactive Settings Menu

Type `menu` at the serial prompt to enter the interactive settings editor. This is a full-screen terminal UI (VT100 compatible) for adjusting all MeshCore node preferences interactively. Exit with **Esc** or **Ctrl+C**.

The menu covers:
- Node name and password
- LoRa radio parameters
- Flood and scoping parameters
- Advert interval settings

---

## Examples: Common Operator Workflows

### Set up a new room for a specific team

```
> room add
  -> OK - room[1] added, id=A1B2C3D4
> room set 1 name MedTeam
> room set 1 pass med-secret-pass
> room set 1 guest med-guest
> room qr 1
meshcore://contact/add?name=MedTeam&public_key=...&type=3
```

Share the QR code or URI with team members.

### Check if a user is active and what they have missed

```
> room status 0
room[0] 'Triage'  posts=15  clients=3:
  [0] admin  A1B2...  last_act=1722345600  unsynced=0
  [1] rw     B2C3...  last_act=1722344000  unsynced=2
  [2] rw     C3D4...  last_act=1722300000  unsynced=8
```

Client [2] has 8 unsynced posts — they have been offline for a while.

### Promote a user to admin

```
> room clients 0
  [0] rw  A1B2C3D4E5F6...
> room setperm 0 A1B2C3D4E5F6 4
  -> OK - room[0] perm set
```

### Switch to STA mode for LAN access

```
> wifi ssid MyOfficeNetwork
> wifi pass MyWiFiPassword
> wifi mode sta
> wifi connect
> wifi status
  Mode: STA  SSID: MyOfficeNetwork  IP: 192.168.1.42
```

Then access the web UI at `http://192.168.1.42`.
