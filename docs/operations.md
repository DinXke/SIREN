# Operations & Troubleshooting

This document covers common operational scenarios, known issues, and their solutions.

---

## Boot Sequence

Understanding what happens at boot helps diagnose problems.

### Normal boot sequence

1. **ESP32-S3 starts** — bootloader runs from flash
2. **SPIFFS mounts** — filesystem with settings and identities is loaded
3. **Radio initialises** — SX1262 configured with persisted radio settings
4. **Rooms load** — all active room slots are loaded; missing keypairs are generated
5. **Peers load** — peer room server list loaded from `/peer_cfg`
6. **WiFi starts** — AP mode (or STA mode if configured)
7. **Web server starts** — listening on port 80
8. **OLED shows boot screen** — "SIREN boot..." then node name + radio settings
9. **Serial banner** — lists available commands at 115200 baud
10. **Advert on boot** — non-stealth rooms send an advert after ~16 seconds

**Normal boot time**: approximately 3-5 seconds until the mesh is operational.

---

## Known Operational Gotchas

### 1. Radio settings persist in SPIFFS — OTA does NOT reset them

When you flash a new firmware via OTA, the radio settings (freq/BW/SF/CR/TX) are loaded from SPIFFS on boot. The firmware's compiled-in defaults (`LORA_FREQ`, `LORA_BW`, etc.) are only used if no settings are found in SPIFFS.

**Consequence**: If a device was previously running firmware with different radio defaults (e.g., an older build), the OTA update will NOT fix the radio settings. The device will continue using whatever was saved in SPIFFS.

**Fix**: After a firmware change that includes new radio defaults:
1. Connect to the web UI → LoRa Settings and set the correct values manually, OR
2. Use the CLI: `set freq 869.618`, `set bw 62.5`, `set sf 8`, `set cr 8`, `set tx_power 22`

The `SIREN_v3_room_server_radio_fix` build (dist: `SIREN_v3_room_server_radio_fix.bin`) was created specifically for this scenario — it force-writes the correct EU868 radio settings to SPIFFS on first boot, regardless of what was there before.

### 2. Stealth mode is ON by default

All rooms default to stealth (no adverts). If users cannot find the room server with their MeshCore companion app, it is probably because stealth is on.

**Fix**: Share the join URI or QR code out-of-band (Web UI → Rooms → QR icon, or `room qr <idx>` in CLI), or turn stealth off (`room stealth <idx> off`).

### 3. Posts are lost on reboot

Messages stored in the post pool are in RAM only. Reboot = all messages lost. Room configuration, identities, and member lists survive reboots (stored in SPIFFS).

**Mitigation**: In production, ensure the device has stable power. Use a UPS or LiPo battery with a USB power bank for field deployments.

### 4. 1% duty cycle limits message rate

EU868 regulations allow at most 1% transmit time. At SF8/BW62.5, a typical message transmission takes ~100-200 ms of airtime. This means a maximum of about 5-10 messages per minute before the duty cycle limit is hit — shared across ALL rooms plus any replication traffic.

**Consequence**: In a high-traffic scenario with multiple active rooms and heavy message volume, some messages may be queued and delayed. This is expected behaviour, not a bug.

### 5. ACK timeouts and push retries

The room server pushes undelivered messages to each member. If a member does not acknowledge a push within the timeout, the server retries up to 3 times. After 3 failures, the server stops pushing to that member until they send activity again.

**Signs of this**: A member's `unsynced` count in `room status` stays high and never decreases. This means the member's radio is not reachable (out of range, off, or on wrong radio settings).

---

## Boot Loops

A boot loop is when the device repeatedly reboots without successfully completing startup.

### Causes

1. **Bad OTA image** — the firmware image is corrupt or incompatible
2. **SPIFFS filesystem corruption** — rare, usually after an abrupt power cut during a write
3. **Hardware fault** — radio initialisation failed (most commonly: no antenna, or SX1262 damaged)
4. **Partition table mismatch** — OTA image written to wrong partition

### Diagnosis (serial available)

Connect serial at 115200 baud. Watch for the boot banner. If you see:
- `radio_init failed` or similar — check antenna; try a full re-flash
- Crash + reset immediately — likely bad firmware; re-flash with last known good image
- SPIFFS errors — try factory reset via full re-flash

### Recovery: OTA rollback

MeshCore uses ESP-IDF OTA with two app partitions. If the new firmware fails to call `esp_ota_mark_app_valid_cancel_rollback()` during boot, the bootloader rolls back to the previous firmware on the next reboot.

**In practice**: The SIREN firmware calls this mark on successful startup. If the device boot-loops, the ESP-IDF bootloader should automatically roll back after a configurable number of failed boot attempts.

### Recovery: Full re-flash (USB available)

1. Download `dist/SIREN_v3_room_server-full-flash.bin`
2. Flash at offset `0x0` using the web flasher (see [Getting Started](getting-started.md))
3. This overwrites everything — SPIFFS is included in the full-flash image

**Warning**: Full re-flash erases all configuration (room identities, member lists, radio settings, WiFi credentials). Have a backup before doing this if you care about the existing room identities.

### Recovery: No USB, OTA-only device (Unit B)

If a device has no working USB and a bad OTA image is installed:
1. The device may still create its WiFi AP if the boot gets far enough
2. Attempt to access `http://192.168.4.1` and use the OTA update page
3. If the device cannot start its WiFi stack, it is unrecoverable without USB access

**This is why we test on Unit A (USB available) before sending any OTA to Unit B.**

---

## Radio Verification

### How to verify radio settings without a screen

If the OLED is unreadable or unavailable, verify radio settings via:

1. **Serial CLI** (if USB available):
   ```
   > radio
     -> freq=869.618 bw=62.5 sf=8 cr=8 tx=22
   ```

2. **Web UI** — connect to the WiFi AP (`http://192.168.4.1`) → LoRa Settings

3. **Second node observation** — a second Heltec running MeshCore (at matching settings) will discover the SIREN room server via advert (if stealth is off) or accept a login (if you join via QR code). If the second node can see/hear the first, radio settings match.

---

## GitHub Self-Update (JES-774)

When the device is in **STA mode** (connected to the internet via WiFi), it can
download and flash its own firmware directly from the GitHub repository.

### Via Web UI

1. Open the web management page → **Firmware Update** card (near the bottom).
2. Click **Controleer op update** — the device fetches
   `dist/version.json` from GitHub and shows whether an update is available.
3. If a newer version is listed, click **Nu bijwerken**.
4. A progress bar shows download progress.  The page auto-refreshes.
5. The device reboots automatically after a successful flash.

### Via CLI (serial or mesh)

```
ota check          # fetch manifest, report available version
ota update         # download + flash (only works after 'ota check')
ota status         # show current state / progress
```

### How it works

| Step | Detail |
|------|--------|
| Manifest | Fetches `https://raw.githubusercontent.com/DinXke/SIREN/multiroom/dist/version.json` over HTTPS |
| Version check | Compares manifest `version` field to compiled-in `FIRMWARE_VERSION` |
| Download | Streams `dist/heltec_v3/SIREN_v3_room_server.bin` (or v4 on V4 hardware) |
| Integrity | SHA-256 of downloaded bytes verified against manifest before any partition change |
| Flash | Written to inactive OTA partition via `esp_ota_*` API |
| Rollback | `esp_ota_mark_app_valid_cancel_rollback()` called in `setup()` — bad images auto-rollback |
| Settings | SPIFFS and NVS are never touched — all channels/keys/prefs survive |

### Failure modes

| Error | Cause | Resolution |
|-------|-------|------------|
| `manifest HTTP 0` / DNS error | Device not in STA mode or no internet | Enable STA + connect WiFi |
| `manifest HTTP 404` | Branch / path changed on GitHub | Check `dist/version.json` is on `multiroom` branch |
| `SHA-256 mismatch` | Download corrupted or wrong file | Retry; check manifest matches committed binary |
| `no inactive OTA partition` | Partition table has no second app slot | Should not occur with `partitions_siren.csv`; re-flash full image |
| `esp_ota_write: 0x102` | Image too large for partition | Firmware grew beyond 3.1 MB — rebuild with size reduction |

---

## OTA Update Checklist

Before sending an OTA update to any device, especially Unit B (no USB):

- [ ] New firmware has been built with `pio run -e SIREN_v3_room_server`
- [ ] Build succeeded with no errors; RAM ≤ 95%, Flash ≤ 95%
- [ ] New firmware tested on Unit A (USB available) — device boots cleanly
- [ ] Serial console confirms correct radio settings after boot
- [ ] Web UI accessible after boot
- [ ] At least one room is active and joinable
- [ ] Backup of Unit B's settings downloaded before updating
- [ ] OTA image is `SIREN_v3_room_server.bin` (NOT the full-flash binary)

---

## Factory Reset

### Via CLI (serial)

```
> reset
```

This erases SPIFFS and reboots. All configuration is lost. The device starts with factory defaults (node name "SIREN", admin password "password", one default room, EU868 radio settings).

### Via Web UI

System → Factory Reset (button in the web management page).

### Via full re-flash

Flash `SIREN_v3_room_server-full-flash.bin` at offset `0x0` — this includes a fresh SPIFFS image.

---

## MeshCore Version Compatibility

SIREN is built on **MeshCore tag `room-server-v1.16.0`**. Important wire-format facts for mixed-version mesh networks:

- **ACK hash size**: v1.16 sends 6-byte ACK hashes; pre-v1.16 sent 4-byte. A v1.16 room server can communicate with pre-v1.16 companion radios (the extra bytes are benign), but ACK de-dup may not work perfectly in mixed-version scenarios.
- **NVS format**: The `SimpleMeshTables` format changed in v1.16 (removed `_acks` array). A device upgrading from v1.15 with persisted tables may de-dup incorrectly until the table clears naturally.

For upgrading the MeshCore firmware version, see [meshcore-upgrade-runbook.md](meshcore-upgrade-runbook.md).

---

## Common Error Messages

### `Err - invalid room idx`
The room index you provided is out of range (must be 0-15) or the room is not active.

### `Err - cannot delete room 0 or invalid idx`
Room 0 is the management room and cannot be deleted.

### `Err - all slots in use`
All 16 room slots are already active. Delete an unused room before adding a new one.

### `Err - pubkey not found or invalid`
The hex public key prefix you provided did not match any known client in the ACL.

### `[SIREN] FORCE_RADIO_PREFS: ...` in serial boot output
The firmware was compiled with `FORCE_RADIO_PREFS=1` (the radio-fix build). This is intentional and will overwrite radio settings on this boot. Flash the normal `SIREN_v3_room_server.bin` afterwards to remove this behaviour.

---

## IRC Chat Web UI

The web management UI at `http://<node-ip>/` includes a link to the **IRC Chat** page (`/chat`). This page requires admin basic-auth (same credentials as the management UI).

### What the chat page shows

- **Kanaal** (channel) dropdown — select the active room to view.
- **Messages pane** — displays all posts for the selected room in chronological order. Author names are resolved from the advert name table; unknown nodes show an 8-character hex prefix. Messages auto-refresh every 4 seconds.
- **Users pane** — shows connected companions with their role colour:
  - Yellow: admin
  - Green: read-write
  - Grey: read-only / guest
  Auto-refreshes every 5 seconds.
- **Post form** — post a message as the room operator (`[OP]`). The post is pushed to all connected companions immediately.

### Security

- All `/chat`, `/api/chat/messages`, `/api/chat/nicks`, and `/api/chat/post` endpoints require admin basic-auth.
- Message text is displayed using `textContent` (never `innerHTML`) to prevent XSS.
- Server-authored posts are stored with the room's own key as author and prefixed with `[OP]`.

### Name resolution

Node names are learned from LoRa advertisements (`onAdvertRecv`). The name table holds up to 32 entries (LRU eviction) and is persisted in SPIFFS at `/names`. It is included in the backup/restore (`GET /api/backup`, `POST /api/restore`).

If a node has never advertised or is in stealth mode, its name appears as an 8-character hex prefix of its public key.

---

## Monitoring in Production

Useful commands to run periodically on a deployed node:

```bash
# Check all rooms and their client/post counts
rooms
room list

# Show recent messages in room 0
msgs 0 10

# Show connected users in room 0
nicks 0

# Post an operator announcement to room 0
say 0 Onderhoud gepland: 14:00-15:00

# Check sync status for a specific room
room status 0

# Check radio performance
radio stats

# Check overall packet statistics
stats
```

There is no automated alerting in the current SIREN firmware. Monitoring is manual.

---

## Backup Frequency Recommendation

For production deployments:
- Take a backup (Web UI → Download Backup) after any configuration change
- Take a weekly backup as a baseline
- Store backups in a secure location (they contain private cryptographic keys)

Loss of room identity keys means users need to re-join the room from scratch (the old room's history is also lost if the device is reset).

**Note on MQTT credentials**: The MQTT password (`mqtt.pass`) is stored in `/mqtt_cfg.json` and is **NOT included in the backup export**. After restoring a backup, reconfigure MQTT credentials manually via web UI or CLI.

---

## MQTT Integration (JES-792)

SIREN room servers optionally publish to an MQTT broker for remote monitoring and future cross-site synchronisation.

### Enabling MQTT via CLI
```
mqtt set host broker.example.com
mqtt set port 8883
mqtt set tls on
mqtt set ca_fp AA:BB:CC:...
mqtt set user siren
mqtt set pass <secret>
mqtt enable
mqtt status
```

### What is published
| Topic | Description |
|---|---|
| `siren/<net_id>/room/<hash>/msg` | Encrypted post envelope (AES-256-CTR) |
| `siren/<net_id>/room/<hash>/meta` | Room metadata (retained) |
| `siren/<net_id>/node/<id>/status` | Online/offline + LWT (retained) |
| `siren/<net_id>/node/<id>/vv` | Post watermarks per room (retained) |

Post content is always encrypted — the broker never sees plaintext. See `docs/mqtt.md` for decryption instructions and the full specification.
