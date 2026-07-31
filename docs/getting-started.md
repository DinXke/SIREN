# Getting Started

This guide walks you through flashing a Heltec LoRa32 V3 with SIREN firmware for the first time, and how to update it later.

---

## Before You Begin

You will need:

- A **Heltec LoRa32 V3** board
- A **LoRa antenna** connected to the SMA port (required — never power on without antenna)
- A **USB-C cable** connected to a PC
- **Chrome or Edge browser** (required for the web flasher)
- The firmware files from the `dist/` folder of this repository

---

## Step 1 — Download the Firmware

From the [SIREN GitHub repository](https://github.com/DinXke/SIREN/tree/multiroom/dist):

- **`SIREN_v3_room_server-full-flash.bin`** — use this for the **first time** you flash a device (full image including bootloader and partition table)
- **`SIREN_v3_room_server.bin`** — use this for **OTA updates** (app image only, no bootloader)

---

## Step 2 — First Flash (New Device)

Use the browser-based ESPTool for the initial flash. This method works on all platforms without installing any software.

### 2a. Open the web flasher

Open **Chrome or Edge** (not Firefox or Safari) and go to:

```
https://espressif.github.io/esptool-js/
```

### 2b. Connect and flash

1. Click **Connect** in the web flasher.
2. A browser dialog will ask you to choose a serial port — select the one that appeared when you plugged in the Heltec (typically **COM3** on Windows, or `/dev/ttyUSB0` on Linux).
3. Click **Flash** (or Erase + Flash if you want a fully clean start).
4. Set the flash offset to **`0x0`** (zero).
5. Click the file picker and select **`SIREN_v3_room_server-full-flash.bin`**.
6. Click **Program** and wait for it to complete (typically 30-60 seconds).

### 2c. Reboot

Once flashing is complete, unplug and re-plug the USB cable (or press the reset button if accessible). The device will boot with the SIREN firmware.

**What you should see:**
- The OLED briefly shows a boot screen, then displays the node name, frequency, and radio settings
- The device creates a WiFi hotspot named `SIREN-<nodename>` (default node name is `SIREN`)
- The serial console (115200 baud) prints a boot banner listing available commands

---

## Step 3 — Verify the Radio Settings

After first flash, confirm the radio settings are correct. SIREN is pre-configured with EU868 defaults:

| Setting | Value |
|---|---|
| Frequency | 869.618 MHz |
| Bandwidth | 62.5 kHz |
| Spreading Factor | SF8 |
| Coding Rate | CR 4/8 |
| TX Power | 22 dBm |

**Verify via serial console** (if USB is available):
```
> radio
  -> freq=869.618 bw=62.5 sf=8 cr=8 tx=22
```

**Verify via OLED**: The display shows `869.618 SF8` on one line and `BW62.5 CR8` on the next.

**Verify via web UI**: Connect to the WiFi hotspot, open `http://192.168.4.1`, go to the LoRa Settings page.

**Verify via second node**: If a second Heltec (running MeshCore companion or room server at matching settings) can see an advertisement from this node, the radio settings are correct.

---

## Step 4 — Connect to the Web UI

1. On your phone or laptop, connect to the WiFi hotspot `SIREN-<nodename>` (default: `SIREN-SIREN`, open network / no password by default).
2. Open a browser and go to `http://192.168.4.1`.
3. The SIREN web management interface loads.

From here you can:
- View all active rooms
- Change room names and passwords
- Configure WiFi (switch to STA mode to join an existing network)
- Download a backup of all settings
- Trigger a firmware update (OTA)

See [WiFi & Web UI](wifi-webui.md) for full details.

---

## Updating Firmware (OTA)

Once SIREN is running, you never need to use USB flash again. Use Over-The-Air (OTA) updates instead.

### Via the Web UI

1. Connect to the device's WiFi (AP mode) or find it on your LAN (STA mode).
2. Open the web management page.
3. Navigate to **System → Firmware Update**.
4. Upload `SIREN_v3_room_server.bin` (the OTA image — not the full-flash binary).
5. Wait for the upload and reboot to complete.

### What OTA preserves

OTA updates install new firmware into the inactive partition and reboot into it. Your settings are preserved:

- Room identities and configuration (SPIFFS: `/room_cfg`, `/identity/`)
- WiFi settings (SPIFFS: `/wifi_sta.json`)
- Radio preferences (SPIFFS: loaded from NVS/CommonCLI prefs)
- Node name, admin password

**Warning for Unit B (glued/no-serial)**: If an OTA image fails to boot (e.g., due to a bug), the device may be unrecoverable. Always test a new build on Unit A (serial available) first and confirm it boots cleanly before sending it to Unit B.

---

## Default Admin Password

The default admin password is **`password`**.

**Change it immediately** after first boot, especially if the device will be accessible over WiFi:

```
> set password <newpassword>
```

Or change it via the Web UI → Settings → Admin Password.

---

## Changing the Node Name

The node name appears in adverts and on the OLED display:

```
> set name MyRoomServer
```

Node names are limited to approximately 20 characters. Shorter is better (fits on screen and in packets).

---

## Quick Reference: First Boot Checklist

- [ ] Antenna connected before powering on
- [ ] Firmware flashed at offset `0x0` using full-flash binary
- [ ] Device reboots and WiFi hotspot `SIREN-*` appears
- [ ] Web UI accessible at `http://192.168.4.1`
- [ ] Radio settings verified: 869.618 / BW62.5 / SF8 / CR4:8 / 22 dBm
- [ ] Admin password changed from default
- [ ] Node name set

---

## Next Step

After the device is running, see [WiFi & Web UI](wifi-webui.md) to learn how to use the built-in management interface, or [CLI Reference](cli-reference.md) if you prefer the command line.
