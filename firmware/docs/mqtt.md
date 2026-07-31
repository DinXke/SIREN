# SIREN MQTT Transport

**JES-792 Phase (a) — Publish-only**

SIREN room servers can optionally publish room events to an MQTT broker. This enables:
- Remote monitoring of room activity (encrypted)
- Integration with hospital IT/SIEM systems
- Foundation for Phase (b) cross-room synchronisation over MQTT + LoRa mesh

MQTT is **off by default** and requires explicit configuration. It is fully independent of the LoRa mesh — LoRa anti-entropy continues to work if MQTT is unavailable.

---

## Security Requirements (non-negotiable)

| Requirement | Implementation |
|---|---|
| No plaintext message content on broker | Post payloads AES-256-CTR encrypted |
| TLS mandatory for non-loopback brokers | `WiFiClientSecure`; plain TCP only for `127.x` / `localhost` |
| Credentials never logged | `mqtt.pass` is write-only in web UI and CLI |
| Credentials excluded from backup | MQTT config is stored separately (`/mqtt_cfg.json`) and never included in the backup export |
| Off by default | `mqtt.enable = false` |

> **Hospital context**: This design satisfies GDPR/NIS2 requirements. The MQTT broker is a "dumb relay" — it forwards ciphertext without being able to read message content.

---

## Encryption Details

Every post published to MQTT is encrypted before transmission.

**Key derivation:**
```
aes_key = HMAC-SHA256(room_prv_key[0..31], "siren-mqtt-v1")
```
The room private key is the first 64 bytes of the room identity stored in SPIFFS. The HMAC uses only the first 32 bytes (seed portion of the Ed25519 key).

**Encryption:**
- Cipher: AES-256-CTR (`mbedtls_aes_crypt_ctr`)
- Nonce: 16 bytes random per message (`esp_fill_random`)
- Input: UTF-8 post text (up to 151 bytes)
- Output: same-length ciphertext

**Envelope format (published JSON):**
```json
{
  "v":   1,
  "r":   "a1b2c3d4",
  "ts":  1722470400,
  "ak":  "0011223344...aabbccdd",
  "nc":  "0102030405060708090a0b0c0d0e0f10",
  "enc": "deadbeef..."
}
```

| Field | Description |
|---|---|
| `v` | Envelope version (always 1 for Phase a) |
| `r` | Room hash: hex of `room_pub_key[0:4]` (8 chars) |
| `ts` | Unix timestamp of the post |
| `ak` | Author public key (64 hex chars, PUB_KEY_SIZE=32 bytes) |
| `nc` | AES-CTR nonce (32 hex chars = 16 bytes) |
| `enc` | AES-256-CTR ciphertext of the post text (hex) |

**Decryption (for authorised monitoring systems):**
1. Export the room backup from the web UI (includes private keys)
2. Extract `room0_id` field (192 hex chars = 96 bytes = prv_key[64] + pub_key[32])
3. Derive AES key: `HMAC-SHA256(id_bytes[0:32], b"siren-mqtt-v1")`
4. Decrypt: `AES-256-CTR(key, nonce, ciphertext)`

---

## Topic Scheme

Base: `siren/<net_id>/...`  (default `net_id` = `"siren"`, configurable)

| Topic | Retain | QoS | Content |
|---|---|---|---|
| `siren/<net_id>/room/<room_hash>/msg` | No | configurable (default 1) | Encrypted post envelope (JSON) |
| `siren/<net_id>/room/<room_hash>/meta` | Yes | 0 | Room metadata JSON `{"name":…,"clients":…,"posts":…}` |
| `siren/<net_id>/node/<node_id>/status` | Yes | 0 | `{"status":"online"/"offline","node":"…","ts":…}` |
| `siren/<net_id>/node/<node_id>/vv` | Yes | 0 | Post watermarks per room `{"<room_hash>":{"posts":N}}` |

- `room_hash` = hex of `pub_key[0:4]` (8 hex chars) for each room identity
- `node_id` = hex of `room0_pub_key[0:4]` (the device's primary identity)
- `status` topic also serves as the **Last Will and Testament (LWT)**: if the node disconnects unexpectedly, the broker publishes `{"status":"offline"}` automatically

---

## Configuration

### Web UI

Navigate to the web management interface (`http://<device-ip>/`) → **MQTT** section.

Fields:
- **Enable** toggle
- **Broker hostname/IP** and **port** (default 8883 for TLS)
- **TLS** on/off (off only permitted for `127.x`/`localhost`)
- **CA Fingerprint** (SHA-1, e.g. `AA:BB:CC:DD:...`): validates server certificate. Leave blank to accept any TLS cert (not recommended for production)
- **Username** / **Password** (password is write-only — never displayed)
- **Client ID** (auto-generated from node name if empty)
- **Network ID** (`net_id` used in all topics, default `siren`)
- **QoS** for `msg` topic (0 or 1)
- **VV heartbeat interval** (seconds, default 60, minimum 30)

### CLI (serial or mesh)

```
mqtt status
mqtt enable
mqtt disable
mqtt set host <broker.example.com>
mqtt set port <port>           (default 8883)
mqtt set tls on|off            (off only for loopback)
mqtt set ca_fp <fingerprint>   (SHA-1, colons optional)
mqtt set user <username>
mqtt set pass <password>       (write-only, not echoed)
mqtt set client_id <id>        (leave empty for auto)
mqtt set net_id <id>           (default: siren)
mqtt set qos 0|1
mqtt set interval <seconds>    (VV heartbeat, 30-3600)
```

### Persistence

Config is stored in SPIFFS at `/mqtt_cfg.json`.
**Note:** MQTT credentials (`mqtt.pass`) are stored in `/mqtt_cfg.json` **only** and are **never** included in the `/api/backup` export. If you restore a backup, you must reconfigure MQTT credentials manually.

---

## Flash / RAM Budget

PubSubClient and mbedtls AES add to firmware size. Budget gate for Phase (a):
- Flash < 60% of device capacity
- RAM < 55% of device capacity

Verify with `pio run -e SIREN_v3_room_server` and check the reported percentages.
If flash approaches 60%, consider disabling unused RadioLib variants (already excluded via build flags).

---

## Broker Setup Example (Mosquitto)

```
# /etc/mosquitto/conf.d/siren.conf
listener 8883
cafile   /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/server.crt
keyfile  /etc/mosquitto/certs/server.key
require_certificate false

allow_anonymous false
password_file /etc/mosquitto/passwd
```

Generate CA fingerprint for the `ca_fp` setting:
```bash
openssl x509 -in /etc/mosquitto/certs/server.crt -fingerprint -sha1 -noout \
  | sed 's/SHA1 Fingerprint=//'
```

---

## Phase Roadmap

| Phase | Status | Description |
|---|---|---|
| **(a) Publish-only** | **This document** | Room server publishes encrypted posts + metadata to MQTT |
| (b) Subscribe + ingest | Pending JES-723/724 | Subscribe to peer topics; dedup with mesh SYNCDAT |
| (c) Cross-transport reconciliation | Pending Phase (b) | VV digest piggybacked on mesh SYNC; health UI |

See `docs/replication-protocol.md` for anti-entropy details (Phases b and c).

---

## Security Sign-off

Before production deployment in a NIS2-regulated environment (hospital):

1. Configure TLS with a valid broker certificate (`ca_fp` set or full CA chain)
2. Use unique broker credentials per SIREN network
3. Rotate room identity keys periodically (requires new backup export + VV reset)
4. Obtain NIS2 sign-off from SecurityEngineer / SecureDevEngineer (see JES-792)
