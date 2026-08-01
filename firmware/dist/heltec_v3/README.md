# SIREN Firmware — Heltec LoRa32 V3

**Target:** Heltec LoRa32 V3 (ESP32-S3, SX1262)
**Environment:** `SIREN_v3_room_server`
**Branch:** `multiroom`
**Build date:** 2026-07-31
**Version:** `v1.0-siren-p2` (Phase 9 — WiFi AP/STA + Web Management UI, MAX_ROOMS=16)

---

## Files

| File | Size | Use |
|------|------|-----|
| `SIREN_v3_room_server.bin` | ~1.1 MB | **OTA update** — upload via web UI at `/update` |
| `SIREN_v3_room_server-full-flash.bin` | ~1.2 MB | **Initial / recovery flash** — esptool or web serial at offset `0x0` |
| `SIREN_v3_room_server_radio_fix.bin` | ~1.1 MB | **OTA — radio settings recovery** — one-shot correction for stale prefs from a prior firmware |

> **IMPORTANT:** Use `SIREN_v3_room_server.bin` for OTA updates.
> Use `SIREN_v3_room_server-full-flash.bin` ONLY for first-time or recovery serial flash at offset `0x0`.
> **Do NOT upload the full-flash binary via the OTA web interface** — it will corrupt the flash and cause a boot loop.

---

## Option A — First-time flash or recovery (web serial / esptool)

Use this when the device has no SIREN firmware yet, or to recover from a boot loop.

1. Go to the ESP web serial flasher: https://espressif.github.io/esptool-js/
2. Connect the Heltec LoRa32 V3 via USB.
3. Select `SIREN_v3_room_server-full-flash.bin`.
4. **Flash offset: `0x0`** (the full-flash image includes bootloader + partition table + app).
5. Flash and reboot.

> **Note:** This image was built with `--flash_mode dio --flash_freq 80m --flash_size 8MB`.

Alternatively via esptool:
```bash
esptool.py --chip esp32s3 --port COM<n> write_flash 0x0 SIREN_v3_room_server-full-flash.bin
```

---

## Option B — OTA update (device already running SIREN)

### Via built-in web interface

The device starts a WiFi AP named `SIREN-<nodename>` on boot (default, open).

1. Connect your PC to the `SIREN-<nodename>` WiFi access point.
2. Open a browser and go to `http://192.168.4.1/update`.
3. Click **Choose file** and select `SIREN_v3_room_server.bin` (not the full-flash binary).
4. Click **Update** and wait for the device to reboot.

If the device is in STA mode and connected to your network, use `http://<device-ip>/update`.

### Via esptool (command line)

```bash
esptool.py --chip esp32s3 --port COM<n> write_flash 0x10000 SIREN_v3_room_server.bin
```

---

## WiFi configuration

### AP mode (default)

On boot the node creates a WiFi hotspot:
- **SSID:** `SIREN-<nodename>` (open by default)
- **IP:** `192.168.4.1`
- **Web UI:** `http://192.168.4.1/`
- **OTA update:** `http://192.168.4.1/update`

To set a WPA2 password for the AP: `wifi ap pass <password>`

### STA mode (connect to existing network)

```
wifi mode sta
wifi ssid <your-ssid>
wifi pass <your-password>    ; omit for open networks
wifi connect
wifi status
```

The device falls back to AP mode automatically if STA connection times out (30 s).

### Full CLI reference

```
wifi mode ap|sta          ; switch AP / STA mode (persists to SPIFFS)
wifi ap ssid <ssid>       ; set AP hotspot SSID
wifi ap pass <pass>       ; set AP WPA2 password (omit for open)
wifi ssid <ssid>          ; set STA target SSID
wifi pass <pass>          ; set STA password (omit for open)
wifi connect              ; reconnect with current credentials
wifi status               ; show current WiFi state and IP
```

---

## Web UI

Served at `http://<device-ip>/` (HTTP Basic Auth: user `admin`, password = `ADMIN_PASSWORD` build flag, default `password`).

- **/** — Node status: name, IP, firmware version, active rooms; WiFi configuration form
- **/update** — OTA firmware upload (use `SIREN_v3_room_server.bin` only)

---

## Partition layout (for reference)

| Name | Type | Subtype | Offset | Size |
|------|------|---------|--------|------|
| nvs | data | nvs | 0x009000 | 20 KB |
| otadata | data | otadata | 0x00e000 | 8 KB |
| app0 (ota_0) | app | ota_0 | 0x010000 | 3264 KB |
| app1 (ota_1) | app | ota_1 | 0x340000 | 3264 KB |
| spiffs | data | spiffs | 0x670000 | 1536 KB |
| coredump | data | coredump | 0x7f0000 | 64 KB |

Dual OTA slots are present — over-the-air updates are safe and will not erase NVS or SPIFFS.

---

## Build stats

| Environment | RAM | Flash |
|---|---|---|
| `SIREN_v3_room_server` | 40.7% (133320 / 327680 B) | 35.3% (1181493 / 3342336 B) |
| `SIREN_v3_room_server_radio_fix` | 40.5% (132864 / 327680 B) | 34.7% (1161037 / 3342336 B) |

---

## Recovery from boot loop ("Invalid image block, can't boot")

If the device is stuck in a boot loop with this error, the bootloader area in flash
is corrupted. This typically happens when the full-flash binary is accidentally
uploaded via the OTA web interface instead of the OTA-only binary.

**To recover:**

1. Connect the board via USB.
2. Flash `SIREN_v3_room_server-full-flash.bin` at offset `0x0`:
   - Web serial: https://espressif.github.io/esptool-js/ — select file, offset `0x0`
   - esptool: `esptool.py --chip esp32s3 --port COM<n> write_flash 0x0 SIREN_v3_room_server-full-flash.bin`
3. Reboot. The device will start normally.

---

## Radio-settings recovery (SIREN_v3_room_server_radio_fix.bin)

Use this image when the device may be running on stale radio settings stored
by a previous firmware (MeshCore OTA does **not** reset SPIFFS prefs).

**What it does on first boot:**

1. Loads existing SPIFFS prefs (`/com_prefs`) — node name, crypto identity, and
   all non-radio settings are preserved.
2. Overwrites freq / SF / BW / CR / TX-power with the SIREN EU868 defaults:
   - Frequency: **869.618 MHz**
   - Bandwidth: **62.5 kHz**
   - Spreading factor: **SF8**
   - Coding rate: **CR 4/8**
   - TX power: **22 dBm**
3. Saves the corrected prefs back to SPIFFS.
4. Continues normal operation — the node is immediately on the correct frequency.

**After confirming the device is on the right frequency**, flash the normal
`SIREN_v3_room_server.bin` to remove the one-shot correction behaviour.

> **Safety:** Both OTA slots are intact — the known-good `SIREN_v3_room_server.bin`
> remains as an immediate rollback target.
