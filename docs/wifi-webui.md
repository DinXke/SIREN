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
- Number of active rooms
- Current WiFi mode and IP address
- Uptime

### Rooms

Shows a table of all active room slots with:
- Room index (0-15)
- Room name
- Number of connected clients
- Number of posts stored
- Stealth status (on/off)
- Per-room action buttons (edit name/password, enable/disable stealth, show QR code)

**Adding a room**: Click "Add Room". A new room slot is created with a default name (`Room<N>`) and the default admin password.

**Editing a room**: Click the Edit icon next to a room to change its name, admin password, and guest password.

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
- **Peers table**: name, pubkey prefix, and last-contact for each configured peer, with per-peer **Sync** and **Del** buttons.
- **Peer toevoegen**: paste the other node's full 64-hex pubkey + an optional name. Input is validated (exactly 64 hex chars) and rejected on duplicate or when the peer list is full.
- **Sync All Nu**: force an immediate sync round to every peer.

| Route | Method | Purpose |
|---|---|---|
| `/api/peers` | GET | JSON: own pubkey + configured peers |
| `/api/peer/add` | POST | `pub=<64hex>&name=<name>` |
| `/api/peer/del` | POST | `idx=<n>` |
| `/api/peer/sync` | POST | `idx=<n>` (omit for all peers) |

Two nodes only sync a given room when **both** hold a room with the **same key** (matched by `room_hash`). Coupling nodes ≠ coupling individual rooms — see [replication-protocol.md](replication-protocol.md#coupling-nodes-in-practice). Equivalent serial CLI: `peer add <hex64> <name>`, `peer del <idx>`, `peer list`, `peer sync`.

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

### Self-Update from GitHub (JES-774, planned)

A future firmware version will allow the device to fetch the latest firmware directly from GitHub and update itself, without manually downloading the file. This feature is in development.

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
