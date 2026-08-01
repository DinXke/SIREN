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
| `SIREN_<target>.bin` | **OTA update** — upload via web UI at `/update` |
| `SIREN_<target>-full-flash.bin` | **First flash / recovery** — esptool or web serial at offset `0x0` |

> **IMPORTANT:** Never upload the `*-full-flash.bin` via the OTA web interface.
> That will overwrite the bootloader and partition table, causing a boot loop.
> Use the plain `.bin` for OTA updates.
