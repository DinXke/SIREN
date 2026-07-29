# SIREN Web Client

An IRC-style web interface for SIREN, a LoRa mesh-based emergency communication system.

## Overview

The SIREN web client is a single-page React application that connects to a Python Flask backend. The backend communicates with a SIREN companion radio node over serial (USB), relaying messages over the LoRa mesh to room servers, channels, and contacts.

**Architecture:** Browser (React) → Flask REST/WS bridge (localhost) → SIREN companion_radio node → LoRa mesh

## Features

- **IRC-style UI** with sidebar channels, main chat pane, user list, and input bar
- **Multiple channel types**: Room servers, group channels, direct messages
- **Dual transport**: Serial (USB) or Web Bluetooth to a SIREN Heltec LoRa32 V3 device
- **Real-time messaging** with optimistic updates and delivery status
- **Locked rooms** with password authentication
- **Message history** (in-memory, RAM-based)

## Requirements

- **Node.js** 18+ (for frontend build)
- **Python** 3.11+ (for backend)
- **SIREN companion_radio node** flashed on a Heltec LoRa32 V3
- Serial USB connection to the companion node

## Installation & Setup

### Quick Start (Linux/macOS)

```bash
cd web-client
./install.sh
```

This will:
1. Create Python virtual environment and install dependencies
2. Install Node.js dependencies and build the React frontend
3. Start the Flask server on `http://127.0.0.1:8760`
4. Print the URL to open in your browser

### Quick Start (Windows)

```cmd
cd web-client
install.bat
```

## Manual Setup

### Backend

```bash
cd web-client/server
python3 -m venv venv
source venv/bin/activate  # On Windows: venv\Scripts\activate
pip install -r requirements.txt
python app.py
```

The server will listen on `http://127.0.0.1:8760`.

### Frontend (Development)

```bash
cd web-client/frontend
npm ci
npm run dev
```

Vite will proxy API calls to `http://127.0.0.1:8760`.

### Frontend (Production Build)

```bash
cd web-client/frontend
npm ci
npm run build
```

Output goes to `server/static/` for Flask to serve.

## Usage

### Connection

The connection panel offers two transport options:

- **Serial**: Select your USB serial port (e.g., `/dev/ttyUSB0` or `COM5`) and click "Connect"
- **Bluetooth**: Click "Scan & Connect" to open the Web Bluetooth device picker (requires secure context: `http://localhost:8760` or HTTPS for remote access)

**Bluetooth bonding** (Windows): Before using Bluetooth, bond the device in **Settings > Bluetooth > Pair device**. Use PIN `123456`. If you see bonding errors, see **Remote Access & Bluetooth** below.

### Chatting

1. **View Channels**: Available rooms, channels, and DMs appear in the left sidebar
2. **Join/Chat**: Click a channel to open it; type messages at the bottom
3. **Locked Rooms**: If a room requires a password, enter it in the dialog
4. **User List**: View active contacts and rooms in the right sidebar

## API Contract

The backend exposes:
- **REST endpoints** for state, channels, messages, connection
- **WebSocket** for real-time events (messages, user updates, login results)

See `API_CONTRACT.md` for the full specification.

## Architecture

See `ARCHITECTURE.md` for detailed architecture, security notes, and protocol information.

## Project Structure

```
web-client/
  frontend/              # React + Vite + TypeScript SPA
    src/
      api.ts            # API client (HTTP + WebSocket)
      main.tsx          # React entry point
      App.tsx           # Main UI component
      components/       # IRC UI components
      hooks/            # Custom React hooks
    package.json
    vite.config.ts
  server/               # Flask backend (to be added)
    static/             # Built frontend served by Flask
  ARCHITECTURE.md       # Architecture decision doc
  API_CONTRACT.md       # REST/WS API specification
  README.md
  install.sh
  install.bat
```

## Development

### Frontend Stack
- React 18, TypeScript, Vite
- CSS Modules for styling
- No external UI library (plain CSS for simplicity)

### Backend Stack
- Python 3.11+, Flask, Flask-Sock (WebSocket)
- pyserial for USB communication
- In-memory state (no database)

## Standalone Single-File Client

`web-client/siren-standalone.html` is a zero-dependency alternative — no Node.js, no Python, no build step.
Open it directly in **Chrome or Edge (desktop)** and connect to your SIREN node over USB serial or Bluetooth.

**When to use it vs the full Flask+React stack:**

| Scenario | Use |
|---|---|
| Quick field access, no laptop setup | `siren-standalone.html` |
| Troubleshooting / hardware bring-up | `siren-standalone.html` |
| Full feature set, persistent config, multi-user | Flask + React stack |

Web Serial and Web Bluetooth are Chrome/Edge desktop only. The file displays a compat banner and disables unavailable transports automatically.

## Web Bluetooth Setup

### Secure Context Requirement

Web Bluetooth requires a **secure context**. This means:

- **✅ Local access works out-of-the-box**: Open the app at `http://localhost:8760` or `http://127.0.0.1:8760` — these are treated as secure contexts by the browser
- **❌ Remote access needs HTTPS**: If you want to access from another machine on your LAN (e.g., `http://192.168.1.100:8760`), start the server with HTTPS

### Enable HTTPS for Remote Access

To use Web Bluetooth from a remote browser on your LAN, start the server with HTTPS:

#### Linux / macOS

```bash
./install.sh --host 0.0.0.0 --https
```

#### Windows

```cmd
install.bat --host 0.0.0.0 --https
```

This will:
1. Check that `openssl` is available (ships with Git for Windows; add `C:\Program Files\Git\usr\bin` to PATH if missing).
2. Generate a self-signed certificate into `.certs/cert.pem` / `.certs/key.pem` (skipped if already present).
3. Start Flask on `https://0.0.0.0:<port>` and print the LAN URL.

**Certificate warning**: The browser will show a security warning because the cert is self-signed. Click **Advanced → Proceed** to continue. This is expected and normal.

**Avoiding the warning with mkcert**: [mkcert](https://github.com/FiloSottile/mkcert) creates locally-trusted certificates without browser warnings. Run `mkcert -install` once, then `mkcert -cert-file .certs/cert.pem -key-file .certs/key.pem <your-lan-ip> localhost 127.0.0.1` and restart the server.

> **SECURITY**: Binding to `0.0.0.0` exposes the SIREN mesh API to your entire LAN with **no authentication**. Only do this on a trusted, private network.

## Notes

- **Security**: The app binds to `127.0.0.1` by default. Do not bind to `0.0.0.0` without authentication.
- **Message History**: Messages are stored in RAM only. Restarting the server clears history.
- **Serial Protocol**: Uses MeshCore companion frame protocol; see `ARCHITECTURE.md` section 5.

## License

[See SIREN repo root for license]

## Support

For issues or questions, refer to the SIREN project issue tracker.
