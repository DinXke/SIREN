# SIREN Firmware — Pre-built Binaries

Pre-built firmware for all supported SIREN hardware targets.
Each target has its own subdirectory with OTA and full-flash images.

## Targets

| Directory | Hardware | MCU | Radio |
|-----------|----------|-----|-------|
| [`heltec_v3/`](heltec_v3/README.md) | Heltec LoRa32 V3 | ESP32-S3 | SX1262 |
| [`heltec_v4/`](heltec_v4/README.md) | Heltec LoRa32 V4 | ESP32-S3 | SX1262 + GC1109 PA |

## Building

Use the build script to rebuild all targets and refresh both subdirectories:

```bash
bash scripts/build-dist.sh
```

The script requires PlatformIO (`pio`) on your PATH and must be run from the
repository root.

## File naming

| File | Use |
|------|-----|
| `SIREN_<target>.bin` | **OTA update** — upload via web UI at `/update`, or downloaded automatically by the device self-update |
| `SIREN_<target>-full-flash.bin` | **First flash / recovery** — esptool or web serial at offset `0x0` |
| `version.json` | **Version manifest** — consumed by the device self-update feature |

> **IMPORTANT:** Never upload the `*-full-flash.bin` via the OTA web interface.
> That will overwrite the bootloader and partition table, causing a boot loop.
> Use the plain `.bin` for OTA updates.

## Self-update (GitHub OTA)

When the device is in STA mode (connected to the internet), it can update itself
directly from this GitHub repository:

1. Open the web management UI → **Firmware Update** card.
2. Click **Controleer op update** — the device fetches `dist/version.json` and
   compares the available version against its own `FIRMWARE_VERSION`.
3. If an update is available, click **Nu bijwerken** — the device downloads the
   binary for its hardware target, verifies the SHA-256 from the manifest, and
   flashes the inactive OTA partition.
4. Settings (SPIFFS channels/keys/prefs + NVS) are never touched.
5. After a successful flash the device reboots into the new firmware automatically.

**Integrity**: SHA-256 of each OTA binary is stored in `version.json`.  The device
rejects any image with a mismatched hash before the boot partition is changed.

**Rollback**: If the new image fails to reach `setup()` successfully (boot loop /
watchdog), ESP-IDF automatically boots the previous image.

### When committing a new firmware release

1. Run `bash scripts/build-dist.sh` — this builds V3 + V4 and regenerates
   `dist/version.json` with updated SHA-256 values.
2. Bump `FIRMWARE_VERSION` in both `variants/heltec_v3/platformio.ini` and
   `variants/heltec_v4/platformio.ini` **before** running the build script.
3. Commit `dist/heltec_v3/*.bin`, `dist/heltec_v4/*.bin`, and `dist/version.json`
   together in the same commit as the firmware source change.
