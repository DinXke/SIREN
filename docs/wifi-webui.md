# WiFi & Web UI Guide

The SIREN firmware includes a built-in web management interface accessible via WiFi. This document explains how to connect to it and what you can do.

---

## WiFi Modes

SIREN supports two WiFi modes:

### AP Mode (Access Point) — Default

The device creates its own WiFi hotspot. No router or existing WiFi needed.

- **SSID**: `SIREN-<nodename>` (e.g., `SIREN-SIREN` for the default node name)
- **Password**: None by default (open network). Can be set via CLI or Web UI.
- **IP address of device**: `192.168.4.1`
- **Web UI**: open `http://192.168.4.1` in your browser

**When to use AP mode**: In the field, when no existing WiFi infrastructure is available. Operators connect directly to the SIREN device's hotspot to manage it.

### STA Mode (Station)

The device connects to an existing WiFi network (router or phone hotspot) as a client. Useful in a fixed installation where a local network exists.

- **IP address**: Assigned by the router's DHCP (check your router admin page or use `wifi status` in the CLI)
- **Web UI**: `http://<assigned-ip>`

**When to use STA mode**: In a hospital or command post with an existing LAN. Multiple operators can access the web UI simultaneously.

---

## Switching WiFi Mode

### Via CLI (serial or mesh)

```
wifi mode ap          — switch to AP mode (creates hotspot)
wifi mode sta         — switch to STA mode (connects to existing network)
wifi ssid <name>      — set the SSID of the network to connect to
wifi pass <password>  — set the WiFi password (leave empty for open network)
wifi connect          — (re)connect to the STA network immediately
wifi status           — print current WiFi status and IP address
```

### Configuring AP credentials

```
wifi ap ssid <name>    — set the hotspot SSID
wifi ap pass <pass>    — set the hotspot password (empty = open)
```

### Via Web UI

The web management interface has a **WiFi Settings** page where you can switch modes and enter credentials using a form.

---

## Accessing the Web UI

1. Connect your phone or laptop to the SIREN WiFi hotspot (AP mode) **or** ensure you are on the same LAN as the SIREN device (STA mode).
2. Open a browser and navigate to:
   - AP mode: `http://192.168.4.1`
   - STA mode: `http://<device-ip>` (check via `wifi status` command or router)
3. The SIREN management dashboard loads.

---

## Web UI Pages

### Dashboard (Home)

Shows:
- Node name and current status
- Firmware version — displayed with the product branding, e.g. `SIREN by DinX v1.10.0`
- Number of active rooms
- Current WiFi mode and IP address
- Uptime

> The branded name (`SIREN by DinX <version>`) is shown consistently everywhere the version appears: the web dashboard, the OTA update card, and the `ver` command / Version request over the MeshCore app. The bare semantic version (`v1.10.0`) is still used internally for OTA update comparison against `dist/version.json`.

### Rooms

Shows a table of all active room slots with:
- Room index (0-15)
- Room name
- Number of connected clients
- Number of posts stored
- Stealth status (on/off)
- Per-room action buttons (edit name/password, enable/disable stealth, show QR code)

**Adding a room**: Click "Add Room". A new room slot is created with a default name (`Room<N>`) and the default admin password.

**Editing a room**: Click the Edit icon next to a room to change its name, admin password, guest password, and advertised location (latitude/longitude).

**Location**: The **Locatie** fields (latitude `-90..90`, longitude `-180..180`, decimal degrees) set the coordinates broadcast in the room's advertisement, so clients/maps can show where the room server is. Leave both at `0` to omit the location from the advert. Equivalent serial CLI: `room set <idx> lat <val>` / `room set <idx> lon <val>`.

**Map pin picker**: Below the Locatie fields the edit form shows an interactive **Kaart** (map). Click anywhere on the map, or drag the pin, to set the coordinates — the latitude/longitude fields update automatically (and vice-versa: typing coordinates re-centers the pin). The map (Leaflet + OpenStreetMap) is loaded from a CDN **by your browser**, not by the device, so no tiles are stored on the ESP32. If the room server's admin page is opened from a browser without internet access the map stays blank; in that case simply type the coordinates into the Locatie fields manually — everything still works. Third-party assets are loaded with Subresource Integrity (SRI) pinning.

**QR Code**: Click the QR icon to display a QR code that MeshCore companion apps can scan to join the room directly (out-of-band join — works even with stealth mode on).

### LoRa Settings

Shows and allows editing:
- Frequency (MHz)
- Bandwidth (kHz)
- Spreading Factor
- Coding Rate
- TX Power (dBm)

**Warning**: Changing LoRa settings affects all rooms simultaneously and must match the companion radio settings of all users. Change with care.

### Peer-koppeling (Multi-room Replication) — JES-816

Couples this node with another SIREN node so their rooms replicate. All routes are behind admin basic-auth.

- **Eigen node pubkey**: the 64-hex public key of this node. Give it to the operator of the other node so they can add you as a peer.
- **Peers table**: name, pubkey prefix, and last-contact for each configured peer, with per-peer **Sync**, **Full**, and **Del** buttons.
- **Peer toevoegen**: paste the other node's full 64-hex pubkey + an optional name. Input is validated (exactly 64 hex chars) and rejected on duplicate or when the peer list is full.
- **Sync All Nu**: force an immediate incremental sync round to every peer.
- **Volledige sync** (per-peer **Full**): force a *full* resync that resends every post so messages from **before** a firmware upgrade replicate again. Normal incremental sync can leave those older posts stranded (stuck per-origin watermark); duplicates are dropped by dedup. Run it on **both** coupled nodes (JES-874).

| Route | Method | Purpose |
|---|---|---|
| `/api/peers` | GET | JSON: own pubkey + configured peers |
| `/api/peer/add` | POST | `pub=<64hex>&name=<name>` |
| `/api/peer/del` | POST | `idx=<n>` |
| `/api/peer/sync` | POST | `idx=<n>` (omit for all peers) |
| `/api/peer/fullsync` | POST | `idx=<n>` (omit for all peers) — full resync, empty VV |

Two nodes only sync a given room when **both** hold a room with the **same key** (matched by `room_hash`). Coupling nodes ≠ coupling individual rooms — see [replication-protocol.md](replication-protocol.md#coupling-nodes-in-practice). Equivalent serial CLI: `peer add <hex64> <name>`, `peer del <idx>`, `peer list`, `peer sync`, `peer fullsync`.

### Advert & Verkeer (Manual advert + live traffic) — JES-868

On the **Netwerk** page, the **Advert & Verkeer** card lets you broadcast on demand and watch the airwaves:

- **Flood advert nu versturen**: immediately floods an advert for every visible (non-stealth) room, so other nodes can discover this node right away instead of waiting for the periodic timer. Stealth rooms are still skipped. The button only queues the request; the actual radio transmit runs on the mesh task, never on the web task (avoids the TX/`self_id` race, cf JES-864).
- **Live ontvangen verkeer**: opens `/rxlog`, a live view of **every** packet the radio receives — including flood traffic **not addressed to this node**. Each row shows age, payload type (ADVERT, TXT, ACK, SYNC/REQ…), route (flood/direct), destination-hash byte, hop count, payload length, and last RSSI/SNR. Auto-refreshes every 2 s (toggle off with the checkbox). **Only packet metadata is shown — message content is never decoded or displayed**, since traffic may be encrypted for other nodes.

| Route | Method | Purpose |
|---|---|---|
| `/api/advert` | POST | Queue a manual flood advert (all non-stealth rooms) |
| `/rxlog` | GET | Live received-traffic page |
| `/api/rxlog` | GET | JSON snapshot of the RX ring (metadata only, newest first) |

The RX ring keeps the last 32 packets in RAM (reset on reboot); `total` in the JSON is a monotonic count of all packets seen since boot.

### Buren / neighbours (JES-869)

On the **Netwerk** page, the **Buren (neighbours)** card shows the direct-hop mesh neighbours this node knows about and lets you probe for more:

- **Discover nu versturen**: sends a zero-hop discovery request. Nearby repeaters and room servers answer within ~60 s and appear in the table. Like the advert button, this only queues the request — the actual radio transmit runs on the mesh task, never on the web task (cf JES-864). Responses are rate-limited (max 4 per 2 minutes) to protect airtime.
- The table lists each neighbour's 4-byte pubkey prefix (`Hex`), advertised `Naam` (falls back to hex), last `SNR`, seconds since last `Gehoord`, `Type` (Repeater / Room server / ?), and `Locatie`. It refreshes every 5 s from `/api/neighbors`.
- **Locatie** (JES-868): if a neighbour's advert carried coordinates (`ADV_LATLON_MASK`), the decoded `lat, lon` is shown as a link that opens OpenStreetMap centred on that node; otherwise a dash (`–`). The last known location is retained even if a later advert from the same node omits it.

Neighbours are also learned passively from every zero-hop advert received, so the list fills over time even without an explicit discover.

| Route | Method | Purpose |
|---|---|---|
| `/api/discover` | POST | Queue a zero-hop neighbour-discovery request |
| `/api/neighbors` | GET | JSON neighbour list (`hex`, `name`, `snr_db`, `ago_s`, `type`, and `lat`/`lon` when the advert carried a location) |

Only 4-byte pubkey prefixes are exposed — never private keys. Equivalent CLI commands: `neighbors`, `discover.neighbors`, `neighbor.remove <hex>`.

### Namen / name table (JES-875)

The node keeps a **name table** mapping each known 4-byte pubkey prefix to a display
name. It is filled automatically from received adverts and from author names in sync
frames, and is used everywhere a node is shown: the rooms/chat view, the nick list,
and the neighbour list. Nodes that never send an advert (for example plain repeaters)
show up only as a hex prefix — so on the **Netwerk** page the **Namen** card lets an
operator label them by hand:

- **Naam opslaan**: enter the node's 8-hex pubkey prefix (as shown in the neighbour
  or rooms view) and a name (max 23 chars) to pin it. Manual entries are marked with a
  star (★) and are **never overwritten by a later advert** and **never evicted** when
  the table fills.
- The table lists each entry's `Hex` prefix and `Naam`, with a delete (✕) button per
  row. It refreshes every 10 s from `/api/names`.
- Once set, the name immediately appears when someone opens the room and in the nick
  list.

| Route | Method | Purpose |
|---|---|---|
| `/api/names` | GET | JSON name table (`hex`, `name`, `manual`) |
| `/api/name/set` | POST | Pin a name (`hex` = 8 hex chars, `name`) |
| `/api/name/del` | POST | Remove an entry (`hex` = 8 hex chars) |

All endpoints require admin auth; the POST routes are CSRF-guarded. Only 4-byte pubkey
prefixes are exposed — never private keys. Equivalent CLI commands: `name list`,
`name set <8hex> <name>`, `name del <8hex>`.

### Advert intervals (JES-868)

On the **Rooms** page the advert-interval card has **two separate settings, both in hours**:

- **Zero-hop advert** (`0.01`–`18` u): how often each visible room advertises to **directly reachable neighbours only** (one hop, cheap). Stored internally in seconds; the field accepts fractional hours (e.g. `0.5` = 30 min).
- **Flood advert** (`0`–`240` u, `0` = off, default `47` u): how often each visible room floods an advert across the **whole network**. This is the expensive network-wide beacon, so it defaults to a long period.

Stealth rooms never advertise regardless of these values. Both are applied immediately and persisted (zero-hop to `room_cfg`, flood via the node prefs file).

| Route | Method | Purpose |
|---|---|---|
| `/api/advert/interval` | POST | Set `zerohop_hours` and/or `flood_hours` (both in hours) |

Equivalent serial CLI: `advert interval <seconds>` (zero-hop) and `set flood.advert.interval <hours>` (flood).

### WiFi Settings

Switch between AP and STA mode; set SSID and password for each mode.

### System

- **Firmware Update (OTA)**: Upload a new `SIREN_v3_room_server.bin` file to update the firmware without connecting a USB cable.
- **Restart**: Reboot the device.
- **Factory Reset**: Erase all SPIFFS data and reboot with factory defaults. **This deletes all room identities, member lists, and settings — irreversible.**

---

## Backup and Restore

SIREN supports exporting all settings to a single JSON file and restoring from it. This is useful for:

- Moving settings to a new device
- Recovery after a factory reset
- Keeping a safe copy of crypto keys

### Download a Backup

From the Web UI → System → **Download Backup**, or via the REST API:

```
GET http://192.168.4.1/api/backup
```

The response is a JSON file containing:
- All room names, passwords, and guest passwords
- All room Ed25519 private and public keys (base64-encoded)
- Active/inactive status of each room slot
- Node preferences (radio settings, node name)

**Security note**: The backup contains private cryptographic keys. Store it securely. Anyone with this file can impersonate your SIREN rooms on the mesh.

### Restore from Backup

From the Web UI → System → **Restore from Backup**, upload the JSON file.

Or via the REST API:

```
POST http://192.168.4.1/api/restore
Content-Type: application/json
Body: <contents of backup JSON>
```

After a successful restore, the device reboots automatically to apply all settings.

---

## OTA Firmware Update

### Via Web UI

1. Download `dist/SIREN_v3_room_server.bin` from the GitHub repository.
2. Open the web management page → **System → Firmware Update**.
3. Click "Choose File" and select the `.bin` file.
4. Click "Update".
5. Wait approximately 30 seconds. The device reboots into the new firmware.
6. Reconnect to the web UI and verify the firmware version shown matches what you uploaded.

### Self-Update from GitHub (JES-774)

The device can fetch the latest firmware directly from GitHub and flash itself:

1. Web UI → **System → Firmware Update** → **Controleer op update** (or CLI `ota check`).
2. If a newer version is available, click **Nu bijwerken** (or CLI `ota update`).
3. The image is downloaded over HTTPS, SHA-256 verified against `dist/version.json`,
   flashed to the inactive OTA partition, then the node reboots. Channels, keys and
   settings (SPIFFS/NVS) are preserved.

### OTA hangs partway ("loopt vast na x %") — heap contention (JES-876)

The HTTPS download needs ~40 KB of contiguous heap for its TLS context. On the
no-PSRAM ESP32, if MQTT is also connected it holds a *second* ~40 KB TLS context,
and the two can exhaust heap and stall the flash partway.

- **Automatic mitigation (v1.10.4+):** starting an OTA update now automatically
  suspends MQTT (drops its broker connection and frees the TLS heap) for the
  duration of the download. MQTT reconnects automatically if the update fails; on
  success the node reboots. This applies to both the web button and CLI `ota update`.
- **If an OTA still stalls** (e.g. heap already fragmented by heavy web-UI use):
  1. Reboot the node (fresh boot = maximum free, least-fragmented heap).
  2. Do **not** open the `/network` page before flashing (it allocates a large
     contiguous buffer that fragments the heap).
  3. Optionally `mqtt disable` via CLI/serial to guarantee the TLS heap is free.
  4. Trigger the update immediately: CLI `ota update` or web **Nu bijwerken**.

---

## WiFi Configuration File

WiFi settings are stored in SPIFFS as `/wifi_sta.json`:

```json
{
  "mode": "ap",
  "ap_ssid": "SIREN-MyNode",
  "ap_pass": "",
  "sta_ssid": "MyRouter",
  "sta_pass": "routerpassword"
}
```

This file survives OTA updates but is deleted by a factory reset.

---

## Captive Portal

When in AP mode, the device runs a DNS server that redirects all DNS queries to `192.168.4.1`. On most mobile phones and laptops, this triggers a "Sign in to network" notification when you connect to the SIREN hotspot, which opens the web UI automatically.

---

## Security Notes

- The web UI has **no authentication** currently. Anyone connected to the WiFi (AP mode) or LAN (STA mode) can access the management interface, including the backup endpoint which contains private keys.
- In AP mode with no hotspot password, anyone nearby can connect.
- For operational security: set an AP password, and use STA mode only on trusted private networks.
- The REST API binds to all interfaces — there is no IP-level restriction.
