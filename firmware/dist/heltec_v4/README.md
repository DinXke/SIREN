# SIREN Firmware — Heltec LoRa32 V4

**Target:** Heltec LoRa32 V4 (ESP32-S3, SX1262 + GC1109 PA)
**Environment:** `SIREN_v4_room_server`
**Branch:** `multiroom`

---

## Files

| File | Use |
|------|-----|
| `SIREN_v4_room_server.bin` | **OTA update** — upload via web UI at `/update` |
| `SIREN_v4_room_server-full-flash.bin` | **Initial / recovery flash** — esptool or web serial at offset `0x0` |

> **IMPORTANT:** Use `SIREN_v4_room_server.bin` for OTA updates.
> Use `SIREN_v4_room_server-full-flash.bin` ONLY for first-time or recovery serial flash at offset `0x0`.
> **Do NOT upload the full-flash binary via the OTA web interface.**

---

## Build this target

```bash
bash scripts/build-dist.sh
```

Or build just V4:

```bash
cd firmware
pio run -e SIREN_v4_room_server
cp .pio/build/SIREN_v4_room_server/firmware.bin dist/heltec_v4/SIREN_v4_room_server.bin
```

---

## Option A — First-time flash or recovery (web serial / esptool)

1. Go to the ESP web serial flasher: https://espressif.github.io/esptool-js/
2. Connect the Heltec LoRa32 V4 via USB.
3. Select `SIREN_v4_room_server-full-flash.bin`.
4. **Flash offset: `0x0`** (the full-flash image includes bootloader + partition table + app).
5. Flash and reboot.

Alternatively via esptool:
```bash
esptool.py --chip esp32s3 --port COM<n> write_flash 0x0 SIREN_v4_room_server-full-flash.bin
```

---

## Option B — OTA update (device already running SIREN)

### Via built-in web interface

The device starts a WiFi AP named `SIREN-<nodename>` on boot (default, open).

1. Connect your PC to the `SIREN-<nodename>` WiFi access point.
2. Open a browser and go to `http://192.168.4.1/update`.
3. Click **Choose file** and select `SIREN_v4_room_server.bin` (not the full-flash binary).
4. Click **Update** and wait for the device to reboot.

---

## Hardware notes (V4 vs V3)

The V4 uses an external **GC1109 PA** for transmit (controlled by SX1262 DIO2).
TX power is configured as 10 dBm at the SX1262 output, yielding ~22 dBm after the PA.
All other radio settings (869.618 MHz / BW62.5 / SF8 / CR4:8) are identical to V3.

The V4 OLED uses SDA=17 / SCL=18 / RESET=21.

---

## Partition layout

Same as V3 — dual OTA slots (3264 KB each), SPIFFS 1536 KB, NVS 20 KB.

| Name | Type | Subtype | Offset | Size |
|------|------|---------|--------|------|
| nvs | data | nvs | 0x009000 | 20 KB |
| otadata | data | otadata | 0x00e000 | 8 KB |
| app0 (ota_0) | app | ota_0 | 0x010000 | 3264 KB |
| app1 (ota_1) | app | ota_1 | 0x340000 | 3264 KB |
| spiffs | data | spiffs | 0x670000 | 1536 KB |
| coredump | data | coredump | 0x7f0000 | 64 KB |
