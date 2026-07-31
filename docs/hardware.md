# Hardware Guide

This document covers the physical hardware that SIREN runs on.

---

## The Device: Heltec LoRa32 V3

SIREN firmware targets the **Heltec LoRa32 V3** (sometimes written as "Heltec WiFi LoRa 32 V3").

### Specifications

| Component | Detail |
|---|---|
| Microcontroller | ESP32-S3FN8 (dual-core, 240 MHz, 8 MB flash, no PSRAM) |
| Radio chip | Semtech SX1262 |
| Display | 0.96" SSD1306 OLED, 128 x 64 pixels |
| USB | USB-C, Silicon Labs CP210x USB-to-Serial |
| WiFi | 802.11 b/g/n (built into ESP32-S3) |
| Bluetooth | BLE 5.0 (built into ESP32-S3) |
| LoRa antenna port | SMA connector |
| WiFi/BLE antenna | U.FL connector (small snap-on) or built-in PCB trace |
| Battery | JST connector for 3.7 V LiPo (optional) |
| Size | ~50 mm x 25 mm x 11 mm |

### Pinout highlights for SIREN

You do not need to wire anything for a standard SIREN room server installation. All connections are internal. The only external connections are:

- **USB-C** — power and (on devices with working USB) serial console
- **SMA antenna port** — connect the LoRa antenna here (**required**, see warning below)
- **Battery connector** — optional, for untethered operation

---

## Antenna Warning

**Never power the device on without an antenna connected to the SMA port.**

The SX1262 radio chip can be damaged by transmitting without a load (antenna). Even if you are not actively transmitting, the firmware may attempt an advertisement or acknowledgement packet at any time.

**What antenna to use:** Use the antenna supplied with your Heltec board (usually a short duck antenna with an SMA connector). For better range, a vertically polarised whip antenna tuned for 868-870 MHz works well.

**Antenna placement:** Place the antenna vertically (upright) for best omnidirectional coverage. Avoid placing the antenna directly against metal surfaces or inside a metal enclosure.

---

## The OLED Display

The Heltec V3's display is 0.96 inches measured diagonally — physically very small (approximately 22 mm x 11 mm viewable area). The firmware shows:

- **Row 0 (top)**: Node name
- **Row 1** (y=20): Frequency and spreading factor (e.g., `869.618 SF8`)
- **Row 2** (y=30): Bandwidth and coding rate (e.g., `BW62.5 CR8`)

**Important display limitations:**

- The display **turns off automatically after 20 seconds** of inactivity to save power. Press the PRG button (GPIO0) to wake it.
- On hardware units where buttons are glued or unusable, the OLED will go dark after 20 seconds and cannot be woken. Use the web UI or radio observation to verify settings instead.
- The text is small on this 0.96" screen; a close-up photo is useful for reading it.

---

## The Two Board Units in This Deployment

There are currently two Heltec units in use:

### Unit A — Development/New Unit (MAC: 9c:13:9e:a3:e4:c0)

- USB-C port working normally
- Can be flashed via USB serial (COM3 on the development machine, Silicon Labs CP210x)
- Can also receive OTA updates over WiFi
- Buttons accessible

### Unit B — Board's "Glued" Unit (older, USB broken)

- USB port non-functional (glued to enclosure)
- **Serial console unavailable** — no command-line access via USB
- OLED readable only briefly at boot (text too small to read easily, buttons glued shut = no wake)
- **OTA over WiFi is the only way to update firmware** — a bad OTA image = unrecoverable brick
- Must be tested with extreme care before sending any OTA update

**Implication for Unit B**: All configuration must be done via the Web UI (port 80 on the AP IP address 192.168.4.1) or via CLI-over-mesh (sending commands from another SIREN node or the companion radio). Test every build on Unit A (serial available) before OTA-flashing Unit B.

---

## USB and Serial

When USB is available:

- **Baud rate**: 115200
- **Serial monitor**: Any standard terminal (e.g., PuTTY, screen, Arduino IDE Serial Monitor, VS Code serial monitor)
- **Line ending**: CR (carriage return) — send commands ending with `\r`

The firmware prints a boot banner with available commands when it starts. Type `menu` + Enter to open the interactive settings menu, or type commands directly (see [CLI Reference](cli-reference.md)).

---

## Power Consumption

| Mode | Typical current |
|---|---|
| Active (radio idle) | ~50 mA at 3.3 V |
| Transmitting (22 dBm) | ~100-130 mA peak |
| With OLED on | add ~15-20 mA |

A 1000 mAh LiPo battery will run the board for approximately 12-18 hours in typical mesh-server operation. USB power (5 V via USB-C) is recommended for permanent installations.

---

## Companion Radio vs. Room Server

SIREN uses two different firmware images for two different purposes:

| Firmware | Device role | Used by |
|---|---|---|
| `siren_room_server` | **Room server** — stores messages, manages rooms | Deployed by infrastructure team |
| `companion_radio_usb` or `companion_radio_ble` | **User terminal** — connects to phone/laptop | End users (journalists, medics, field teams) |

This document covers the **room server** hardware. End-user companion nodes use identical hardware but different firmware and do not run SIREN room server code.

---

## Next Step

Now that you know the hardware, move to [Getting Started](getting-started.md) to flash the firmware and get your first room server running.
