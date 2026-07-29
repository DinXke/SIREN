# SIREN Firmware — OTA Package

**Target:** Heltec LoRa32 V3 (ESP32-S3, SX1262)
**Environment:** `SIREN_v3_room_server`
**Branch:** `multiroom`
**Build date:** 2026-07-28
**Version:** `v1.0-siren-p1` (Phase 1, MAX_ROOMS=16)

---

## Files

| File | Size | Use |
|------|------|-----|
| `SIREN_v3_room_server.bin` | ~1.1 MB | **OTA update** — upload via web flasher or ElegantOTA |
| `SIREN_v3_room_server-full-flash.bin` | ~1.2 MB | **Initial / full flash** — use for first-time flashing via web serial or esptool |

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

- RAM: 40.5% (132864 / 327680 bytes)
- Flash: 34.7% (1160805 / 3342336 bytes)

Plenty of headroom for future phases.
