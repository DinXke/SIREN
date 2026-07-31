# Glossary

Definitions of every technical term used in SIREN and MeshCore documentation.

---

## A

**ACK (Acknowledgement)**
A short packet sent by a receiver to confirm that a data packet was received. In MeshCore, the room server sends ACKs when it receives a user's message; the user's companion radio sends ACKs when it receives a pushed post.

**ACL (Access Control List)**
A list of known users (clients) and their permission levels for a specific room. Each room has its own ACL. New users are added to the ACL when they first log in.

**Admin password**
The password required to log in to a SIREN room with administrator privileges (ability to manage members, read the ACL, etc.). Separate from the guest password.

**Advert / Advertisement**
A broadcast packet that a MeshCore node sends to announce its presence on the mesh. Contains the node's public key, name, and optional GPS coordinates. SIREN rooms default to stealth mode (no adverts); adverts can be enabled per room.

**Anti-entropy replication**
A synchronisation technique where two or more nodes periodically compare their data and exchange what each is missing, gradually converging to a consistent state. Used in SIREN for multi-node room server replication (Phase 5+).

**AP mode (Access Point)**
A WiFi operating mode where the SIREN device creates its own hotspot. Other devices connect to it directly. The default mode for SIREN. IP address: `192.168.4.1`.

**Airtime**
The amount of time a LoRa packet occupies the radio channel. Determined by spreading factor, bandwidth, coding rate, and payload size. Longer airtime = longer range, but slower throughput. EU868 limits total airtime to 1% of the time (duty cycle).

---

## B

**Bandwidth (BW)**
A LoRa radio parameter that determines the frequency range used for transmission (in kHz). Narrower bandwidth = longer range and better sensitivity, but slower data rate. SIREN default: 62.5 kHz.

**BLE (Bluetooth Low Energy)**
A short-range wireless protocol built into the ESP32-S3. Used by the SIREN companion radio (not the room server) to connect to web clients without a USB cable. The Web Bluetooth API in Chrome/Edge enables this.

**Boot loop**
When a device repeatedly reboots without successfully completing startup. Common causes: bad firmware image, hardware fault, or filesystem corruption.

**Bootloader**
The low-level software that runs on the ESP32-S3 at power-on before the main firmware. Responsible for selecting which firmware partition to boot and performing OTA rollback if needed.

---

## C

**CLI (Command-Line Interface)**
Text-based commands typed into the serial console or sent over the mesh. SIREN supports a rich CLI for managing rooms, peers, radio settings, and more. See [CLI Reference](cli-reference.md).

**CLI-over-mesh**
A mechanism for sending CLI commands via encrypted direct messages on the LoRa mesh, rather than over USB serial. Enables remote configuration without physical access to the device.

**Coding Rate (CR)**
A LoRa radio parameter that adds forward error correction. Expressed as a ratio: 4/5, 4/6, 4/7, or 4/8. Higher coding rate (4/8) means more error correction overhead but better reliability in noisy environments. SIREN default: 4/8 (written as `CR8`).

**Companion radio**
A Heltec device running MeshCore `companion_radio` firmware. Acts as a user terminal: connects to a phone/laptop via USB or BLE, and relays messages over LoRa to room servers and other contacts. Different firmware from the SIREN room server.

**Contact**
In MeshCore, any other node whose public key is known and stored. Contacts can be room servers, companion radios, or other node types. Adding a room as a contact enables joining it.

---

## D

**dBm (decibel-milliwatt)**
A unit for measuring radio power. 22 dBm = approximately 158 milliwatts. The SX1262's maximum legal output in EU868 for SIREN's use case.

**Dest-hash**
The first few bytes of a room's public key, used to route incoming packets to the correct room slot without revealing the full public key. In multiroom mode, multiple rooms may share the same dest-hash prefix (a "collision"), requiring the firmware to try decrypting with each matching room's key.

**Direct message (DM)**
A message encrypted for one specific recipient (using their public key). Used for room login, post delivery, and CLI-over-mesh. Distinguished from flood messages which are broadcast to everyone.

**Duty cycle**
The fraction of time a radio device is allowed to transmit. EU868 regulations impose a 1% duty cycle on most frequency channels. SIREN's firmware enforces this limit automatically.

---

## E

**Ed25519**
An elliptic-curve digital signature algorithm used by MeshCore for node/room identity. Each node or room has an Ed25519 keypair: a public key (shared with others) and a private key (kept secret). Messages are signed with the private key and verified with the public key.

**ESP32-S3**
The microcontroller used in the Heltec LoRa32 V3. Dual-core, 240 MHz, 8 MB flash, no PSRAM. Manufactured by Espressif. The "S3" variant adds USB support and improved BLE.

**EU868**
The European radio frequency band allocated for low-power devices, around 863-870 MHz. SIREN operates at 869.618 MHz within this band. Requires no radio licence for operation within duty-cycle limits.

**esptool**
A Python-based tool (and web-based version at espressif.github.io/esptool-js) for flashing firmware to ESP32 devices over USB serial.

---

## F

**Firmware**
The software that runs directly on the Heltec hardware. SIREN firmware is the C++ program compiled with PlatformIO and flashed to the ESP32-S3. Distinct from the web client software.

**Flash (storage)**
The non-volatile storage on the ESP32-S3 chip (8 MB). Contains the firmware, bootloader, partition table, and SPIFFS filesystem. Not the same as RAM.

**Flash (action)**
The act of writing new firmware to a device's flash storage. Can be done via USB (esptool/web flasher) or OTA (over the air, via WiFi).

**Flood**
A packet transmission mode in MeshCore where the packet is broadcast to all nodes in range, and each receiver re-broadcasts it (up to a hop limit). Used for adverts and some control messages. Distinguished from direct messages.

**Flood scoping**
A mechanism to limit flood propagation to nodes that share a common "region" or "scope" key. Prevents adverts from leaking beyond a defined area.

---

## G

**Guest password**
An optional secondary password for a SIREN room that grants limited (guest-level) access. If set, users who provide the guest password instead of the main password are granted read-only access with telemetry restricted.

---

## H

**Heltec LoRa32 V3**
The specific development board SIREN runs on. Contains an ESP32-S3, SX1262 LoRa radio, SSD1306 OLED display, and USB-C port. See [Hardware Guide](hardware.md).

---

## I

**Identity**
In MeshCore, a node's cryptographic identity: an Ed25519 keypair (private key + public key). In SIREN multiroom, each room slot has its own independent identity. Stored in SPIFFS.

---

## J

**Join URI**
A URI in the format `meshcore://contact/add?name=<n>&public_key=<hex>&type=3` that, when opened in a MeshCore companion app, adds the room as a contact. Allows joining stealth rooms without discovery via adverts.

---

## K

**Keypair**
A pair of cryptographic keys: a public key (shareable) and a private key (secret). In SIREN, each room has its own Ed25519 keypair. The public key is the room's address on the mesh.

---

## L

**LoRa**
Long Range — a proprietary radio modulation technique by Semtech. Designed for low-power, long-range transmission of small data payloads. Uses chirp spread spectrum (CSS) modulation.

**LoRaWAN**
A network protocol layer built on top of LoRa, typically used with cloud gateways. SIREN does NOT use LoRaWAN — it uses LoRa directly via MeshCore, without any central gateway or cloud.

---

## M

**MeshCore**
The open-source LoRa mesh networking firmware by RippleBiz. Provides packet routing, encryption, contact management, and the room server protocol. SIREN is built on top of MeshCore. GitHub: [github.com/ripplebiz/MeshCore](https://github.com/ripplebiz/MeshCore).

**Meshtastic**
A different open-source LoRa mesh project. **Not compatible with MeshCore.** Completely separate ecosystem.

**Multiroom**
The SIREN extension to MeshCore that allows one device to host up to 16 independent virtual room servers simultaneously. Each room has its own identity and operates independently on the mesh.

---

## N

**Node**
A single Heltec device on the mesh. Can be a room server (running SIREN), a companion radio (running companion_radio firmware), a repeater, or another MeshCore-compatible device.

**NUS (Nordic UART Service)**
A Bluetooth GATT service originally defined by Nordic Semiconductor, used as a simple bidirectional serial pipe over BLE. MeshCore uses NUS to communicate between companion radios and web clients.

**NVS (Non-Volatile Storage)**
An ESP-IDF key-value storage system that persists data across reboots in a dedicated flash partition. MeshCore uses NVS (via the `CommonCLI` preferences system) for radio settings, node name, admin password, and mesh parameters.

---

## O

**OLED**
Organic Light-Emitting Diode display. The small 0.96" 128x64 pixel screen on the Heltec board. Driven by an SSD1306 chip. Turns off after 20 seconds of inactivity (can be woken by pressing the PRG button, if accessible).

**OTA (Over-The-Air)**
A firmware update delivered over WiFi, without needing a USB cable. SIREN supports OTA updates via the web management interface. Uses ESP-IDF's OTA mechanism with two app partitions for rollback safety.

---

## P

**Partition table**
A map of how the ESP32's 8 MB flash is divided: bootloader, NVS, OTA slot 0, OTA slot 1, SPIFFS, etc. Must be included in the full-flash image. Not changed by OTA updates.

**Peer**
In SIREN replication context: another SIREN room server node that this node replicates messages with. Configured via `peer add`. Up to 8 peers supported.

**PlatformIO**
The build system and IDE extension used to compile SIREN firmware. Uses INI-based configuration (`platformio.ini`). Target environment: `SIREN_v3_room_server`.

**Post**
A message stored in a SIREN room. Contains: author identity, timestamp, and text (max ~151 characters). Stored in RAM in the global post pool. Lost on reboot.

**Post pool**
The global array of `PostInfo` structures shared across all rooms (`_post_pool[128]`). Each entry's `room_idx` field identifies which room it belongs to.

**PRG button**
The "Program" button on the Heltec board (GPIO0). Press to wake the OLED from auto-off. Hold during boot to enter download mode (for USB flashing). May be glued down on some deployed units.

**Public key**
The shareable half of an Ed25519 keypair. A room's public key is 32 bytes (64 hex characters). It serves as the room's address on the mesh. Included in the join URI/QR code.

---

## Q

**QR code**
A machine-readable barcode encoding the room join URI. Shown on the Web UI per-room. Scan with a MeshCore companion app to add the room as a contact. Works for stealth rooms (out-of-band join).

---

## R

**RAM**
Random Access Memory — volatile storage that loses its contents on power-off or reboot. SIREN stores the post pool and active room state in RAM. The ESP32-S3 has approximately 512 KB of available RAM.

**Repeater**
A MeshCore node whose primary function is to receive and re-transmit packets, extending the mesh's range. SIREN room servers can be configured with repeater-like forwarding settings.

**Room server**
A MeshCore node that acts as a chat room host: stores posts, manages member lists, and pushes messages to members. SIREN's primary function. One physical device can host up to 16 room servers simultaneously (multiroom).

**RoomSlot**
The C++ structure in SIREN firmware that holds one virtual room server's data: identity keypair, name, passwords, ACL, stealth flag, and timing state.

---

## S

**SF (Spreading Factor)**
A LoRa parameter from SF7 to SF12. Higher SF = longer range and better sensitivity, but slower data rate and more airtime. SIREN default: SF8.

**SPIFFS (SPI Flash File System)**
A small FAT-like filesystem stored in a dedicated partition of the ESP32's flash. Used by SIREN to persist room identities, room configuration, WiFi settings, and peer lists. Survives OTA updates; erased by factory reset or full re-flash.

**STA mode (Station)**
A WiFi operating mode where the SIREN device connects to an existing WiFi network (router). Alternative to AP mode. IP address assigned by DHCP.

**Stealth mode**
A room configuration option (default: ON) that suppresses advertisement broadcasts. Stealth rooms are invisible to passive LoRa listeners; users must receive the join URI or QR code out-of-band to join.

**SX1262**
The LoRa radio chip from Semtech used in the Heltec LoRa32 V3. Supports 150-960 MHz, up to +22 dBm output power. The radio connected to the SMA antenna port.

**SYNCREQ / SYNCDAT / SYNCEND**
The three packet types used in SIREN's planned anti-entropy replication protocol. SYNCREQ initiates a sync by sending a version vector; SYNCDAT carries missing posts; SYNCEND closes the sync exchange.

---

## T

**Telemetry**
Sensor data (temperature, battery voltage, etc.) that a room server can include in responses to admin queries. Guests have telemetry access restricted by default.

**TX power**
The radio transmit power in dBm. Higher power = longer range, more current draw. SIREN default: 22 dBm (near maximum for SX1262 in EU868).

---

## V

**Version vector**
A data structure mapping each known room's identifier to the highest message sequence number seen. Used in replication to determine which messages are missing from a peer node.

---

## W

**Web Serial**
A browser API (Chrome/Edge only) that allows web pages to communicate with USB serial devices. Used by the standalone HTML client to connect to a companion radio.

**Web Bluetooth**
A browser API (Chrome/Edge only) that allows web pages to connect to Bluetooth Low Energy devices. Used by both web clients to connect to a BLE-capable companion radio. Requires a secure context (localhost or HTTPS).

---

## X

**X25519**
A Diffie-Hellman key exchange algorithm used by MeshCore to establish shared secrets for encrypted communication between nodes. Works together with Ed25519 identities.
