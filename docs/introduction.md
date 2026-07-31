# Introduction to SIREN

This document explains what SIREN is, what problem it solves, and the key technologies it uses — written for someone who has never worked with radio communication systems before.

---

## What problem does SIREN solve?

Imagine a major disaster: a flood, a chemical spill, or a power outage that takes out the mobile phone network. The emergency services, hospitals, and volunteer teams all need to communicate, but there is no Internet and no working mobile signal.

SIREN — **Shared Incident Radio Emergency Network** — is a self-contained, infrastructure-free communication system for exactly this scenario. It lets teams send text messages over radio, without relying on mobile towers, WiFi infrastructure, or the Internet.

Think of it like **WhatsApp group chats, but running entirely over radio waves with no servers and no Internet required**.

---

## The three layers of SIREN

### Layer 1 — LoRa radio

**LoRa** (short for Long Range) is a type of radio signal designed for sending small amounts of data over long distances using very little power.

A good analogy: imagine shouting a message in a very specific way that carries further than normal speech, but is also quieter — so you can run on a small battery for a very long time. LoRa works in a similar way. A typical LoRa module can reach several kilometres in open terrain (and further with antennas on high points).

SIREN devices transmit in the **EU868 band** — a set of radio frequencies around 869 MHz that are legally available for low-power devices in the Netherlands and most of Europe. No radio licence is needed for operation within the duty-cycle limits.

**Duty cycle**: LoRa is a shared medium. EU868 regulations require that a device transmit at most 1% of the time (1 second per 100 seconds). SIREN's firmware enforces this automatically.

### Layer 2 — MeshCore

**MeshCore** is the open-source software framework that runs on the hardware and handles the radio networking. It provides:

- **Packet routing**: messages hop from device to device to reach their destination, like a chain of people passing a note
- **Encryption**: all messages are encrypted with Ed25519/X25519 cryptography — only the intended recipient can read them
- **Contact discovery**: devices advertise themselves on the radio so other devices can find them and store their contact details
- **Flood vs. direct**: MeshCore supports both "broadcast to everyone" (flood) and "send to one specific device" (direct/DM) messaging

You can think of MeshCore as the "network stack" — the layer that turns raw radio signals into reliable, encrypted messages between named contacts.

### Layer 3 — SIREN

SIREN is built on top of MeshCore and adds **room server** functionality. A room server is like an IRC chat server or a WhatsApp group — it holds a list of members and stores/distributes messages to all of them.

A single SIREN device can host **up to 16 virtual room servers** simultaneously, each with its own identity, name, password, and member list. This is the SIREN "multiroom" feature.

---

## How does a message flow?

Here is the journey of a text message in SIREN:

```
[User on phone/laptop]
    │  types message in app (web client or MeshCore companion app)
    │
[Companion radio node]  (Heltec device running companion_radio firmware)
    │  encodes message → LoRa packet
    │  transmits over 869 MHz radio
    │
          ~~~~~ LoRa radio waves ~~~~~
    │
[SIREN room server node]  (Heltec device running SIREN firmware)
    │  receives LoRa packet
    │  decrypts and authenticates sender
    │  stores message in RAM
    │  pushes message to all other logged-in members
    │
          ~~~~~ LoRa radio waves ~~~~~
    │
[Other companion radios]  belonging to other members
    │  receive the message
    │  display it in the app
    │
[Other users on their phones/laptops]
```

Key insight: **the SIREN node in the middle is the "server"**. It is a small hardware device (not a cloud service) that stores messages in memory and routes them to all members. If the device loses power, messages in RAM are lost — but all configuration (room settings, member lists, identity keys) is saved to flash storage (SPIFFS) and survives reboots.

---

## The hardware at a glance

SIREN runs on the **Heltec LoRa32 V3** — a small development board (roughly the size of a credit card) that contains:

- An **ESP32-S3** microcontroller (the "brain")
- An **SX1262** LoRa radio chip
- A small **0.96" OLED display**
- A **USB-C port** (for power and programming)
- Connectors for an external **LoRa antenna** and a **WiFi/BLE antenna**
- Built-in **LiPo battery connector** (optional)

The board looks like this:

```
 ┌─────────────────────────┐
 │  [SMA antenna port]     │  ← connect LoRa antenna here
 │                         │
 │  ┌──────────────────┐   │
 │  │  OLED display    │   │
 │  │  (128 x 64 px)   │   │
 │  └──────────────────┘   │
 │                         │
 │  ESP32-S3 + SX1262      │
 │                         │
 │  [USB-C port]           │  ← power / serial
 │  [Battery connector]    │  ← optional LiPo
 └─────────────────────────┘
```

---

## What MeshCore is NOT

To avoid confusion:

- MeshCore is **not** Meshtastic (a similar but incompatible LoRa mesh system)
- MeshCore is **not** a WiFi mesh — it uses LoRa radio, not WiFi
- SIREN messages are **not** compatible with standard walkie-talkies or analogue radios
- SIREN does **not** need an Internet connection at any point during normal operation

---

## Key numbers to remember

| Parameter | Value | Notes |
|---|---|---|
| Frequency | 869.618 MHz | EU868 band, channel 7 |
| Bandwidth | 62.5 kHz | Narrower = longer range, slower data rate |
| Spreading Factor | SF8 | Higher SF = longer range, slower |
| Coding Rate | CR 4/8 | More error correction |
| TX Power | 22 dBm | ~158 mW, near legal max for SX1262 |
| Duty cycle | 1% | Legal limit, firmware enforced |
| Max post text | 151 characters | Per message |
| Max rooms per node | 16 | Configurable at build time |
| Post storage | RAM only | Lost on power loss; config persists |

---

## Next step

Once you understand these concepts, move on to [Hardware Guide](hardware.md) to learn about the physical device and its constraints.
