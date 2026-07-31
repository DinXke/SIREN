# SIREN Firmware — OTA Package

**Target:** Heltec LoRa32 V3 (ESP32-S3, SX1262)
**Environment:** `SIREN_v3_room_server`
**Branch:** `multiroom`
**Build date:** 2026-07-31
**Version:** `v1.0-siren-p1` (Phase 1, MAX_ROOMS=16)

---

## Files

| File | Size | Use |
|------|------|-----|
| `SIREN_v3_room_server.bin` | ~1.1 MB | **OTA update** — normal production image |
| `SIREN_v3_room_server-full-flash.bin` | ~1.2 MB | **Initial / full flash** — first-time flash via web serial or esptool |
| `SIREN_v3_room_server_radio_fix.bin` | ~1.1 MB | **OTA — radio settings recovery** — one-shot correction for stale prefs from a prior firmware |

---

## Option A — First-time flash (web serial flasher)

Use this when the device has no SIREN firmware yet, or when you want a clean wipe.

1. Go to a web serial flasher, e.g. the ESP Web Tools flasher or MeshCore web flasher.
2. Connect the Heltec LoRa32 V3 via USB.
3. Select `SIREN_v3_room_server-full-flash.bin`.
4. **Flash offset: `0x0`** (the full-flash image includes bootloader + partition table + app).
5. Flash and reboot.

> **Note:** This image was built with `--flash_mode qio --flash_freq 80m --flash_size 8MB`.
> If your flasher asks for these settings, use them.

---

## Option B — OTA update (device already running SIREN/MeshCore)

Use this to update a device that is already running and connected to WiFi.

### Via AsyncElegantOTA (built-in web interface)

1. Connect the device to your WiFi network (configure SSID/password via serial CLI).
2. Open a browser and go to `http://<device-ip>/update`.
3. Click **Choose file** and select `SIREN_v3_room_server.bin`.
4. Click **Update** and wait for the device to reboot.

### Via esptool (command line)

```bash
esptool.py --chip esp32s3 --port COM<n> write_flash 0x10000 SIREN_v3_room_server.bin
```

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
| `SIREN_v3_room_server` | 40.5% (132864 / 327680 B) | 34.7% (1160805 / 3342336 B) |
| `SIREN_v3_room_server_radio_fix` | 40.5% (132864 / 327680 B) | 34.7% (1161037 / 3342336 B) |

Plenty of headroom for future phases.

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

**After confirming the device is on the right frequency** (observable via a second
node or the OTA-AP liveness check), flash the normal `SIREN_v3_room_server.bin`
to remove the one-shot correction behaviour.

> **Safety:** Both OTA slots are intact — the known-good `SIREN_v3_room_server.bin`
> remains as an immediate rollback target.
