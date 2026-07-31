# Web Clients

SIREN includes two browser-based chat interfaces that end users and operators can use to communicate through a SIREN room server:

1. **`siren-standalone.html`** — a single self-contained HTML file, no installation needed
2. **React web client** — a full-featured app (React + Flask backend), requires installation

---

## Which One Should I Use?

| Situation | Recommended client |
|---|---|
| Quick field access, no laptop setup | `siren-standalone.html` |
| Troubleshooting, hardware bring-up | `siren-standalone.html` |
| Remote access from phone via WiFi | `siren-standalone.html` (Serial) or React (REST/WS) |
| Full feature set, multi-user, persistent history | React + Flask stack |
| Web Bluetooth access | Either (both support BLE) |

---

## Standalone HTML Client (`siren-standalone.html`)

### What it is

A single HTML file (`web-client/siren-standalone.html`) that contains everything — all JavaScript, CSS, and protocol logic — in one file. Open it in Chrome or Edge, plug in a SIREN companion radio over USB (or pair via Bluetooth), and start chatting.

**No installation required. No Node.js. No Python. No server.**

### Requirements

- **Chrome or Edge** desktop browser (not Firefox, not Safari, not mobile browsers)
  - Web Serial API and Web Bluetooth API are required — only available in Chromium-based browsers
- A **SIREN companion radio** node (Heltec running `companion_radio` firmware) connected via:
  - **USB serial** (recommended), or
  - **Bluetooth** (requires OS-level bonding on Windows — see below)

**Note**: The standalone HTML client connects to a **companion radio** (not directly to a room server). The companion radio relays your messages over LoRa to the SIREN room server.

### How to use it

1. Download or open `web-client/siren-standalone.html` (from the repo or served from anywhere).
2. Connect your companion radio Heltec to USB.
3. Open the HTML file in Chrome or Edge.
4. In the connection panel:
   - Choose **Serial** → click **Connect** → select your COM port → click **Open**
   - Or choose **Bluetooth** → click **Scan & Connect**
5. Wait for the handshake to complete (shows "Connected").
6. Available rooms/channels appear in the sidebar. Click one to join.
7. Type messages at the bottom and press Enter to send.

### Transport options in the standalone client

#### Serial (Web Serial)

- Requires USB cable from PC to the companion radio's USB-C port
- Works on any OS where Chrome/Edge can access the COM port
- On Windows, the Silicon Labs CP210x driver must be installed (usually auto-installed by Windows Update)
- Select the correct COM port from the browser dialog

#### Bluetooth (Web Bluetooth)

- Requires Bluetooth to be enabled on the PC and on the companion radio (companion_radio_ble firmware)
- **Windows bonding requirement**: Before connecting, bond the device in Windows Settings:
  1. Open **Settings → Bluetooth & devices → Add device**
  2. Select the Heltec device from the list
  3. Enter PIN **`123456`** when prompted
  4. Once bonded, Chrome can connect without re-entering the PIN
- The companion radio must be running the BLE-capable firmware (`companion_radio_ble`)
- **Note**: SIREN room servers do NOT have Bluetooth — BLE is only on the companion radio

### Secure context requirement for Bluetooth

Web Bluetooth requires a "secure context":
- **Local file**: `file:///path/to/siren-standalone.html` — works if Chrome flags allow it, but serial is more reliable
- **HTTP on localhost/127.0.0.1**: secure context (works)
- **HTTPS on any host**: secure context (works)
- **HTTP on a LAN IP (e.g., 192.168.x.x)**: NOT a secure context → Bluetooth is disabled

If Bluetooth is greyed out or shows a warning about secure context, either access via `http://localhost` or use HTTPS.

---

## React Web Client

### What it is

A full-featured chat application: a React frontend + Python Flask backend. The Flask server connects to the companion radio (via USB serial or Bluetooth) and exposes a REST + WebSocket API. The browser app consumes this API.

Architecture:
```
Browser (React app) → Flask server (localhost:8760) → Companion radio → LoRa mesh → SIREN room server
```

### Requirements

- Python 3.11 or newer
- Node.js 18 or newer (for building the frontend)
- A SIREN companion radio connected via USB

### Installation

#### Linux / macOS

```bash
cd web-client
./install.sh
```

This installs Python dependencies, builds the React frontend, and starts the Flask server.

#### Windows

```cmd
cd web-client
install.bat
```

The server starts on `http://127.0.0.1:8760`. Open this URL in any browser (Chrome, Firefox, Safari — no restrictions like the standalone client).

### Manual installation

```bash
# Backend
cd web-client/server
python3 -m venv venv
source venv/bin/activate   # Windows: venv\Scripts\activate
pip install -r requirements.txt
python app.py

# Frontend (development mode, in a separate terminal)
cd web-client/frontend
npm ci
npm run dev
```

### Connecting

1. Open `http://127.0.0.1:8760` in your browser.
2. In the connection panel, choose:
   - **Serial**: Select the COM port of your companion radio and click Connect.
   - **BLE**: Click "Scan & Connect" (see BLE notes in standalone client section above).
3. Once connected, available rooms and channels appear in the left sidebar.

### Features vs. standalone client

| Feature | Standalone HTML | React + Flask |
|---|---|---|
| No installation | Yes | No (Python + Node.js required) |
| Works in Firefox/Safari | No | Yes (frontend only; Flask required) |
| Persistent message history | No (tab close = lost) | Yes (server holds history while running) |
| Multi-user access | No (single browser) | Yes (multiple browsers can connect to the same Flask server) |
| BLE transport | Yes | Yes |
| Serial transport | Yes (Web Serial) | Yes (pyserial) |

### Enabling HTTPS for Remote Access

By default, Flask binds to `127.0.0.1` (localhost only). To access from another machine on your LAN (e.g., from a phone):

```bash
# Linux / macOS
./install.sh --host 0.0.0.0 --https

# Windows
install.bat --host 0.0.0.0 --https
```

This generates a self-signed TLS certificate in `.certs/` and starts Flask with HTTPS. The browser will show a security warning (expected for self-signed certs) — click **Advanced → Proceed**.

**Security note**: Binding to `0.0.0.0` exposes the SIREN API (with no authentication) to your entire LAN. Only do this on a trusted private network.

---

## BLE Transport Details (Both Clients)

The BLE transport implements the MeshCore NUS (Nordic UART Service) protocol:

- **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **TX characteristic**: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` (app → device)
- **RX characteristic**: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` (device → app)

The handshake sequence on connect:
1. App sends `CMD_APP_START` (code 1)
2. Device responds with firmware version and device info
3. App sends `CMD_DEVICE_QUERY` (code 22) to get channels and contacts
4. App sends `CMD_GET_CONTACTS` (code 4) and `CMD_GET_CHANNEL` (code 31) to enumerate

If you see a "handshake timeout" error when connecting via BLE, the most common cause is:
- Bluetooth bonding not completed (Windows: bond in Settings first, PIN 123456)
- Wrong BLE protocol codes (fixed in commit dc3b9e11)
- Device is running a firmware version that changed the NUS protocol

---

## Troubleshooting

### "Web Serial not available" or greyed out

- Use Chrome or Edge desktop (not Firefox, Safari, or mobile Chrome)
- On Linux, you may need to add your user to the `dialout` group: `sudo usermod -a -G dialout $USER` then log out and back in

### Cannot select the COM port

- Install the Silicon Labs CP210x driver for the Heltec board
- Check Device Manager (Windows) or `ls /dev/tty*` (Linux/macOS) to find the port
- Try a different USB cable (some cables are charge-only with no data lines)

### BLE connection fails with "User cancelled" error

This is a Chrome/Windows OS limitation. The error occurs when:
- The BLE device is not bonded in Windows Settings
- A previous connection was not properly disconnected

**Fix**:
1. In Windows Settings → Bluetooth → remove the Heltec device if listed
2. Add it again (pair with PIN 123456)
3. Retry the connection in the browser

### Messages not appearing

- Check that the companion radio and the SIREN room server are using the same radio settings (frequency, bandwidth, spreading factor, coding rate)
- Confirm you are joined to the room (the room's login password was accepted)
- Check the SIREN room server's serial console (`room status 0`) to see if your client is listed and how many posts are pending

### Flask server won't start

- Check that Python 3.11+ is installed: `python --version`
- Activate the virtual environment before running: `source venv/bin/activate`
- Check that `requirements.txt` was installed: `pip install -r requirements.txt`

### BLE works on standalone but not React client

Verify that:
1. You're accessing the React app at `http://localhost:8760` (not a LAN IP without HTTPS)
2. The BLE protocol constants in the React client match the firmware (see `web-client/frontend/src/ble/types.ts`)
