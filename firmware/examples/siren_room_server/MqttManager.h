#pragma once

#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include "MyMesh.h"

#define MQTT_CONFIG_PATH "/mqtt_cfg.json"

/**
 * MqttManager — Phase (a): MQTT publish-only transport for SIREN room server.
 *
 * Publishes encrypted post envelopes, room metadata, node status (with LWT),
 * and version-vector heartbeats to a configurable MQTT broker.
 *
 * Security properties:
 *  - OFF by default (mqtt.enable = false).
 *  - TLS via WiFiClientSecure (mandatory for non-loopback; setInsecure() used when
 *    no CA fingerprint is configured — still encrypted transport).
 *  - Post payloads are AES-256-CTR encrypted; key derived from room private key.
 *    The broker sees only ciphertext. Zero plaintext messages are published.
 *  - mqtt.pass is never logged, never exported in backup (write-only credential).
 *
 * Topic scheme (net_id configurable, default "siren"):
 *   siren/<net_id>/room/<room_hash>/msg   QoS-configurable, new post envelope
 *   siren/<net_id>/room/<room_hash>/meta  retained, room metadata
 *   siren/<net_id>/node/<node_id>/status  retained + LWT, online/offline
 *   siren/<net_id>/node/<node_id>/vv      retained, per-room post watermarks
 *
 * room_hash = hex(room_pub_key[0:4])  — 8 hex chars
 * node_id   = hex(room0_pub_key[0:4]) — 8 hex chars
 *
 * Encryption:
 *   key   = first 32 bytes output of HMAC-SHA256(prv_key[0:32], "siren-mqtt-v1")
 *   cipher = AES-256-CTR(key, nonce_16, plaintext_json)
 *   envelope = {"v":1,"r":"<room_hash>","ts":<ts>,"ak":"<pubkey_hex>",
 *               "nc":"<nonce_hex>","enc":"<ciphertext_hex>"}
 *
 * CLI commands (handled via "mqtt ..." prefix in main.cpp):
 *   mqtt status
 *   mqtt enable | disable
 *   mqtt set host <hostname>
 *   mqtt set port <port>
 *   mqtt set tls on|off
 *   mqtt set ca_fp <sha1_fingerprint>   (20 hex bytes, colons optional)
 *   mqtt set user <username>
 *   mqtt set pass <password>            (write-only — not echoed)
 *   mqtt set client_id <id>
 *   mqtt set net_id <id>
 *   mqtt set qos 0|1
 *   mqtt set interval <seconds>         (VV heartbeat, 30-3600)
 */

class MqttManager {
public:
  struct Config {
    bool     enable;
    char     host[128];
    uint16_t port;
    bool     tls;
    char     ca_fp[64];        // SHA-1 fingerprint, e.g. "AA:BB:CC:..." (optional)
    char     user[64];
    char     pass[128];        // WRITE-ONLY — loaded/saved locally, never exported
    char     client_id[64];
    char     net_id[32];
    uint8_t  qos;              // 0 or 1 (default 1)
    uint16_t pub_interval_sec; // VV heartbeat interval (default 60 s)
  };

private:
  MultiRoomMesh&    _mesh;
  Config            _cfg;
  WiFiClientSecure  _wifi_client;
  PubSubClient      _mqtt;
  bool              _started;
  unsigned long     _last_connect_attempt;
  unsigned long     _last_vv_publish;
  char              _last_error[64];
  volatile bool     _ota_suspend;   // JES-876: while true, drop TLS + skip reconnect (frees ~40KB for OTA)

  static const uint32_t RECONNECT_INTERVAL_MS = 30000UL;

  void loadConfig();
  void saveConfig();

  bool connectBroker();
  void publishStatus(bool online);
  void publishMeta(int room_idx);
  void publishVV();

  String roomTopic(int room_idx, const char* suffix);
  String nodeTopic(const char* suffix);

  // Build encrypted post envelope payload.
  // Returns hex-JSON envelope string or "" on error.
  String buildPostEnvelope(int room_idx, uint32_t timestamp,
                           const uint8_t* author_pub, const char* text);

  // Derive 32-byte MQTT encryption key for room_idx (uses room prv_key).
  void deriveMqttKey(int room_idx, uint8_t* key_out_32);

public:
  explicit MqttManager(MultiRoomMesh& mesh);

  /** Call after the_mesh.begin() and SPIFFS is mounted. */
  void begin();

  /** Call every loop iteration. Non-blocking. */
  void loop();

  /**
   * Called by MyMesh::addPost() after a post is stored.
   * Publishes encrypted envelope to siren/<net_id>/room/<room_hash>/msg.
   */
  void onPostAdded(int room_idx, uint32_t timestamp,
                   const uint8_t* author_pub, const char* text);

  bool isEnabled()   const { return _cfg.enable; }
  bool isConnected()       { return _mqtt.connected(); }

  /**
   * JES-876: suspend/resume MQTT during an OTA download. When suspended, the
   * broker connection is dropped (freeing the ~40 KB WiFiClientSecure TLS heap)
   * and loop() will not reconnect. This prevents two concurrent TLS contexts
   * (MQTT + OTA HTTPS download) from exhausting heap on the no-PSRAM ESP32,
   * which otherwise stalls the OTA flash partway ("hangs at X%").
   */
  void setOtaSuspend(bool s);
  bool isOtaSuspended() const { return _ota_suspend; }
  const Config& getConfig() const { return _cfg; }
  const char*   getLastError() const { return _last_error; }

  /** Handle "mqtt ..." CLI commands. Returns true if consumed. */
  bool handleMqttCommand(const char* args, char* reply);

  // Called from WebManager to build the MQTT section of the status page.
  String buildWebSection();
};

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
