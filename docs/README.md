# SIREN Documentation

**SIREN — Shared Incident Radio Emergency Network**

Welcome. This is the documentation index for SIREN. If you are completely new to this project, start at the top of this list and work your way down. Each document builds on the previous one.

---

## Learning Path (Start Here)

### Level 1 — Understanding the Basics

| Document | What you will learn |
|---|---|
| [Introduction](introduction.md) | What SIREN is, what LoRa radio is, what MeshCore is — with plain-language analogies |
| [System Overview](system-overview.md) | The complete system structure in diagrams: all hardware, software, and communication paths shown schematically |
| [Hardware Guide](hardware.md) | The physical device (Heltec LoRa32 V3), antennas, and known constraints of the board units |

### Level 2 — Getting the System Running

| Document | What you will learn |
|---|---|
| [Getting Started](getting-started.md) | How to flash a device for the first time, how to apply firmware updates over the air (OTA), what the default radio settings are |
| [WiFi & Web UI](wifi-webui.md) | How to connect to the device's built-in web interface, configure WiFi, manage rooms, and back up/restore settings |

### Level 3 — Day-to-Day Operation

| Document | What you will learn |
|---|---|
| [CLI Reference](cli-reference.md) | Every command available in the serial console and over the mesh, with examples |
| [Web Clients](web-clients.md) | How to use the browser-based chat interfaces (standalone HTML file and the full React app) |
| [Operations & Troubleshooting](operations.md) | Boot loops, OTA recovery, radio settings persistence, known gotchas |

### Level 4 — Deep Dives

| Document | What you will learn |
|---|---|
| [Multiroom Architecture](architecture.md) | How SIREN runs multiple virtual chat rooms on one device — for developers and advanced operators |
| [Replication Protocol](replication-protocol.md) | How multiple room-server nodes keep their message histories in sync |
| [MeshCore Upgrade Runbook](meshcore-upgrade-runbook.md) | How to upgrade the underlying MeshCore firmware to a new upstream release |

### Reference

| Document | What you will find |
|---|---|
| [Glossary](glossary.md) | Definitions of every technical term used in SIREN and MeshCore |
| [CONTRIBUTING-DOCS.md](CONTRIBUTING-DOCS.md) | Rules for keeping this documentation up to date |

---

## Quick-Start Summary

**Have a new Heltec device and want to get it running in 10 minutes?**

1. Download `dist/SIREN_v3_room_server-full-flash.bin` from this repository.
2. Open [espressif.github.io/esptool-js](https://espressif.github.io/esptool-js) in Chrome or Edge.
3. Connect your Heltec via USB, flash at offset `0x0`.
4. The device reboots, creates a WiFi hotspot named `SIREN-<name>`.
5. Connect to the hotspot, open `http://192.168.4.1` in your browser.
6. Done — you have a running room server.

See [Getting Started](getting-started.md) for full step-by-step instructions.

---

## Project Overview

```
SIREN repo (branch: multiroom)
├── firmware/                   # MeshCore-based firmware for Heltec LoRa32 V3
│   └── examples/siren_room_server/   # SIREN-specific room server code
├── web-client/                 # Browser-based chat interfaces
│   ├── siren-standalone.html   # Zero-dependency single-file client
│   ├── frontend/               # React + TypeScript SPA
│   └── server/                 # Python Flask bridge (serial/BLE → REST/WS)
├── dist/                       # Pre-built firmware binaries
│   ├── SIREN_v3_room_server.bin        # OTA update image
│   └── SIREN_v3_room_server-full-flash.bin  # Initial flash image (offset 0x0)
├── docs/                       # This documentation
└── scripts/                    # Helper scripts (key generation, MeshCore upgrade)
```

---

## Getting Help

- **Issue tracker**: [github.com/DinXke/SIREN/issues](https://github.com/DinXke/SIREN/issues)
- **Contributing to docs**: See [CONTRIBUTING-DOCS.md](CONTRIBUTING-DOCS.md)
