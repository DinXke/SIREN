# SIREN Web Client

An IRC-style web interface for SIREN, a LoRa mesh-based emergency communication system.

## Overview

The SIREN web client is a single-page React application that connects to a Python Flask backend. The backend communicates with a SIREN companion radio node over serial (USB), relaying messages over the LoRa mesh to room servers, channels, and contacts.

**Architecture:** Browser (React) → Flask REST/WS bridge (localhost) → SIREN companion_radio node → LoRa mesh

## Features

- **IRC-style UI** with sidebar channels, main chat pane, user list, and input bar
- **Multiple channel types**: Room servers, group channels, direct messages
- **Serial connection** to a SIREN Heltec LoRa32 V3 device
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

1. **Connect to Device**: Select your serial port (e.g., `/dev/ttyUSB0` or `COM5`) and click "Connect"
2. **View Channels**: Available rooms, channels, and DMs appear in the left sidebar
3. **Join/Chat**: Click a channel to open it; type messages at the bottom
4. **Locked Rooms**: If a room requires a password, enter it in the dialog
5. **User List**: View active contacts and rooms in the right sidebar

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

## Notes

- **Security**: The app binds to `127.0.0.1` by default. Do not bind to `0.0.0.0` without authentication.
- **Message History**: Messages are stored in RAM only. Restarting the server clears history.
- **Serial Protocol**: Uses MeshCore companion frame protocol; see `ARCHITECTURE.md` section 5.

## License

[See SIREN repo root for license]

## Support

For issues or questions, refer to the SIREN project issue tracker.
