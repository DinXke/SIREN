#ifdef ESP32
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)

#include "MqttManager.h"
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

/* ------------------------------------------------------------------ */
/*  Constructor                                                         */
/* ------------------------------------------------------------------ */
MqttManager::MqttManager(MultiRoomMesh& mesh)
  : _mesh(mesh), _mqtt(_wifi_client),
    _started(false), _last_connect_attempt(0), _last_vv_publish(0),
    _ota_suspend(false)
{
  memset(&_cfg, 0, sizeof(_cfg));
  _cfg.enable          = false;
  _cfg.port            = 8883;
  _cfg.tls             = true;
  _cfg.qos             = 1;
  _cfg.pub_interval_sec = 60;
  strncpy(_cfg.net_id, "siren", sizeof(_cfg.net_id) - 1);
  _last_error[0] = 0;
}

/* ------------------------------------------------------------------ */
/*  Config persistence                                                  */
/* ------------------------------------------------------------------ */
void MqttManager::loadConfig() {
  File f = SPIFFS.open(MQTT_CONFIG_PATH, "r");
  if (!f) return;
  String raw = f.readString();
  f.close();

  // Hand-rolled JSON field extractor (no ArduinoJson dependency)
  auto extractStr = [&](const char* key, char* dest, size_t dest_len) {
    String k = String("\"") + key + "\":\"";
    int start = raw.indexOf(k);
    if (start < 0) return;
    start += k.length();
    int end = raw.indexOf("\"", start);
    if (end < 0) return;
    String val = raw.substring(start, end);
    val.replace("\\\"", "\"");
    val.replace("\\\\", "\\");
    strncpy(dest, val.c_str(), dest_len - 1);
    dest[dest_len - 1] = 0;
  };

  char tmp[16] = {};
  extractStr("enable",   tmp, sizeof(tmp));
  _cfg.enable = (tmp[0] == '1' || strcmp(tmp, "true") == 0);

  extractStr("host",      _cfg.host,      sizeof(_cfg.host));
  extractStr("port",      tmp,            sizeof(tmp));
  if (tmp[0]) _cfg.port = (uint16_t)atoi(tmp);

  extractStr("tls",       tmp,            sizeof(tmp));
  _cfg.tls = !(strcmp(tmp, "0") == 0 || strcmp(tmp, "false") == 0);

  extractStr("ca_fp",     _cfg.ca_fp,     sizeof(_cfg.ca_fp));
  extractStr("user",      _cfg.user,      sizeof(_cfg.user));
  extractStr("pass",      _cfg.pass,      sizeof(_cfg.pass));  // loaded but never exported
  extractStr("client_id", _cfg.client_id, sizeof(_cfg.client_id));
  extractStr("net_id",    _cfg.net_id,    sizeof(_cfg.net_id));

  extractStr("qos",      tmp, sizeof(tmp));
  if (tmp[0]) _cfg.qos = (uint8_t)(atoi(tmp) != 0 ? 1 : 0);

  extractStr("interval", tmp, sizeof(tmp));
  if (tmp[0]) _cfg.pub_interval_sec = (uint16_t)atoi(tmp);

  // Defaults for empty fields
  if (_cfg.port == 0) _cfg.port = 8883;
  if (_cfg.net_id[0] == 0) strncpy(_cfg.net_id, "siren", sizeof(_cfg.net_id) - 1);
  if (_cfg.pub_interval_sec < 30) _cfg.pub_interval_sec = 60;
}

void MqttManager::saveConfig() {
  // Helper to escape JSON strings
  auto esc = [](const char* s, char* out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 3 < out_len; i++) {
      if (s[i] == '"')       { out[j++] = '\\'; out[j++] = '"'; }
      else if (s[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
      else                   { out[j++] = s[i]; }
    }
    out[j] = 0;
  };

  char h[132], u[68], p[264], ci[68], ni[36];
  esc(_cfg.host,      h,  sizeof(h));
  esc(_cfg.user,      u,  sizeof(u));
  esc(_cfg.pass,      p,  sizeof(p));  // pass stored locally only — never in backup
  esc(_cfg.client_id, ci, sizeof(ci));
  esc(_cfg.net_id,    ni, sizeof(ni));

  File f = SPIFFS.open(MQTT_CONFIG_PATH, "w");
  if (!f) { Serial.println("[MQTT] saveConfig: SPIFFS open failed"); return; }
  f.printf("{\"enable\":\"%d\","
           "\"host\":\"%s\",\"port\":\"%d\","
           "\"tls\":\"%d\",\"ca_fp\":\"%s\","
           "\"user\":\"%s\",\"pass\":\"%s\","
           "\"client_id\":\"%s\",\"net_id\":\"%s\","
           "\"qos\":\"%d\",\"interval\":\"%d\"}",
           _cfg.enable ? 1 : 0,
           h, (int)_cfg.port,
           _cfg.tls ? 1 : 0, _cfg.ca_fp,
           u, p,
           ci, ni,
           (int)_cfg.qos, (int)_cfg.pub_interval_sec);
  f.close();
}

/* ------------------------------------------------------------------ */
/*  Topic builders                                                      */
/* ------------------------------------------------------------------ */
// room_hash = hex of first 4 bytes of room pub_key
String MqttManager::roomTopic(int room_idx, const char* suffix) {
  const uint8_t* pub = _mesh.getRoomPubKey(room_idx);
  if (!pub) return "";
  char room_hash[9];
  snprintf(room_hash, sizeof(room_hash), "%02x%02x%02x%02x",
           pub[0], pub[1], pub[2], pub[3]);
  String t = "siren/";
  t += _cfg.net_id;
  t += "/room/";
  t += room_hash;
  t += "/";
  t += suffix;
  return t;
}

// node_id = hex of first 4 bytes of room-0 pub_key
String MqttManager::nodeTopic(const char* suffix) {
  const uint8_t* pub = _mesh.getRoomPubKey(0);
  if (!pub) return "";
  char node_id[9];
  snprintf(node_id, sizeof(node_id), "%02x%02x%02x%02x",
           pub[0], pub[1], pub[2], pub[3]);
  String t = "siren/";
  t += _cfg.net_id;
  t += "/node/";
  t += node_id;
  t += "/";
  t += suffix;
  return t;
}

/* ------------------------------------------------------------------ */
/*  Key derivation                                                      */
/* ------------------------------------------------------------------ */
void MqttManager::deriveMqttKey(int room_idx, uint8_t* key_out_32) {
  // Access room identity via backup API: prv_key is first 64 bytes of room identity bytes
  uint8_t id_buf[96] = {};
  size_t n = _mesh.getRoomIdentityBytes(room_idx, id_buf, sizeof(id_buf));
  if (n < 32) { memset(key_out_32, 0, 32); return; }

  // HMAC-SHA256(prv_key[0:32], "siren-mqtt-v1")
  const uint8_t label[] = "siren-mqtt-v1";
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md, id_buf, 32, label, sizeof(label) - 1, key_out_32);

  // Zero the private key material from stack
  memset(id_buf, 0, sizeof(id_buf));
}

/* ------------------------------------------------------------------ */
/*  Encrypted post envelope                                             */
/* ------------------------------------------------------------------ */
String MqttManager::buildPostEnvelope(int room_idx, uint32_t timestamp,
                                       const uint8_t* author_pub,
                                       const char* text) {
  // Derive AES key for this room
  uint8_t aes_key[32];
  deriveMqttKey(room_idx, aes_key);
  if (aes_key[0] == 0 && aes_key[1] == 0 && aes_key[2] == 0) {
    // Key derivation failed (room not active / no private key)
    return "";
  }

  // Random 16-byte nonce/counter for AES-256-CTR
  uint8_t nonce[16] = {};
  esp_fill_random(nonce, sizeof(nonce));

  // Plaintext JSON to encrypt — room context, author, text
  char plain[MAX_POST_TEXT_LEN + 32];
  // Just the text as UTF-8 plaintext (structured fields are in the outer envelope)
  strncpy(plain, text, MAX_POST_TEXT_LEN);
  plain[MAX_POST_TEXT_LEN] = 0;
  size_t plain_len = strlen(plain);

  // AES-256-CTR encrypt
  uint8_t cipher[MAX_POST_TEXT_LEN + 1] = {};
  {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, aes_key, 256);
    uint8_t stream_block[16] = {};
    size_t  nc_off = 0;
    uint8_t nonce_ctr[16];
    memcpy(nonce_ctr, nonce, 16);
    mbedtls_aes_crypt_ctr(&aes, plain_len, &nc_off, nonce_ctr, stream_block,
                          (const uint8_t*)plain, cipher);
    mbedtls_aes_free(&aes);
  }

  // Zero sensitive material
  memset(aes_key, 0, sizeof(aes_key));

  // Hex-encode nonce and ciphertext
  const auto toHex = [](const uint8_t* src, size_t n, char* out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
      out[i * 2]     = h[src[i] >> 4];
      out[i * 2 + 1] = h[src[i] & 0x0f];
    }
    out[n * 2] = 0;
  };

  char nc_hex[33];
  char ci_hex[MAX_POST_TEXT_LEN * 2 + 2];
  char ak_hex[PUB_KEY_SIZE * 2 + 1];
  toHex(nonce, 16, nc_hex);
  toHex(cipher, plain_len, ci_hex);
  toHex(author_pub, PUB_KEY_SIZE, ak_hex);

  // room_hash = hex of first 4 bytes of room pub_key
  const uint8_t* rpub = _mesh.getRoomPubKey(room_idx);
  char r_hex[9] = "00000000";
  if (rpub) {
    snprintf(r_hex, sizeof(r_hex), "%02x%02x%02x%02x",
             rpub[0], rpub[1], rpub[2], rpub[3]);
  }

  // Build outer JSON envelope (unencrypted metadata + ciphertext)
  // {"v":1,"r":"<room_hash>","ts":<ts>,"ak":"<pubkey_hex>","nc":"<nonce_hex>","enc":"<cipher_hex>"}
  String env = "{\"v\":1,\"r\":\"";
  env += r_hex;
  env += "\",\"ts\":";
  env += (unsigned long)timestamp;
  env += ",\"ak\":\"";
  env += ak_hex;
  env += "\",\"nc\":\"";
  env += nc_hex;
  env += "\",\"enc\":\"";
  env += ci_hex;
  env += "\"}";

  return env;
}

/* ------------------------------------------------------------------ */
/*  Status / meta / VV publish helpers                                  */
/* ------------------------------------------------------------------ */
void MqttManager::publishStatus(bool online) {
  String topic = nodeTopic("status");
  if (topic.isEmpty()) return;

  String payload;
  if (online) {
    payload = "{\"status\":\"online\",\"node\":\"";
    payload += _mesh.getNodeName();
    payload += "\",\"ts\":";
    payload += (unsigned long)millis() / 1000UL;
    payload += "}";
  } else {
    payload = "{\"status\":\"offline\"}";
  }
  _mqtt.publish(topic.c_str(), payload.c_str(), /*retained=*/true);
}

void MqttManager::publishMeta(int room_idx) {
  if (!_mesh.isRoomActive(room_idx)) return;
  String topic = roomTopic(room_idx, "meta");
  if (topic.isEmpty()) return;

  String payload = "{\"name\":\"";
  payload += _mesh.getRoomName(room_idx);
  payload += "\",\"clients\":";
  payload += _mesh.getRoomClientCount(room_idx);
  payload += ",\"posts\":";
  payload += _mesh.getRoomPostCount(room_idx);
  payload += "}";
  _mqtt.publish(topic.c_str(), payload.c_str(), /*retained=*/true);
}

void MqttManager::publishVV() {
  // Version-vector: current post counts per active room (Phase (a) watermark)
  // Full VV reconciliation added in Phase (c) when JES-723/724 land
  String topic = nodeTopic("vv");
  if (topic.isEmpty()) return;

  String payload = "{";
  bool first = true;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!_mesh.isRoomActive(i)) continue;
    const uint8_t* pub = _mesh.getRoomPubKey(i);
    if (!pub) continue;
    char r_hex[9];
    snprintf(r_hex, sizeof(r_hex), "%02x%02x%02x%02x", pub[0], pub[1], pub[2], pub[3]);
    if (!first) payload += ",";
    payload += "\"";
    payload += r_hex;
    payload += "\":{\"posts\":";
    payload += _mesh.getRoomPostCount(i);
    payload += "}";
    first = false;
  }
  payload += "}";
  _mqtt.publish(topic.c_str(), payload.c_str(), /*retained=*/true);
}

/* ------------------------------------------------------------------ */
/*  Broker connect                                                       */
/* ------------------------------------------------------------------ */
bool MqttManager::connectBroker() {
  if (_cfg.host[0] == 0) {
    strncpy(_last_error, "no host configured", sizeof(_last_error) - 1);
    return false;
  }

  // Limit blocking time: a TCP+TLS connect to an unreachable broker can block
  // the Arduino loop for the full socket timeout, triggering the Task WDT.
  // 4 s is safely under the default 5 s TWDT threshold (JES-864).
  _wifi_client.setTimeout(4);          // socket read/write timeout (seconds)
  _wifi_client.setHandshakeTimeout(4); // TLS handshake timeout (seconds)

  // TLS configuration
  // NOTE: ESP32 WiFiClientSecure (SDK >=2.x) removed setFingerprint().
  // CA PEM certificate can be loaded from SPIFFS /mqtt_ca.pem for cert validation.
  // Without a CA cert, TLS encryption is active but the server certificate is NOT
  // validated — susceptible to MITM. Load a CA cert for production (Phase c).
  if (_cfg.tls) {
    // Attempt to load CA cert from SPIFFS (optional — user must upload /mqtt_ca.pem)
    File ca_file = SPIFFS.open("/mqtt_ca.pem", "r");
    if (ca_file) {
      String pem = ca_file.readString();
      ca_file.close();
      if (pem.length() > 64) {
        // setCACert stores a reference — copy to heap-stable storage
        static char _ca_pem_buf[4096];
        strncpy(_ca_pem_buf, pem.c_str(), sizeof(_ca_pem_buf) - 1);
        _ca_pem_buf[sizeof(_ca_pem_buf) - 1] = 0;
        _wifi_client.setCACert(_ca_pem_buf);
        Serial.println("[MQTT] TLS: using CA cert from /mqtt_ca.pem");
      } else {
        _wifi_client.setInsecure();
        Serial.println("[MQTT] WARN: /mqtt_ca.pem too short — cert not validated (TLS active)");
      }
    } else {
      // No CA cert file — TLS active but cert not validated
      _wifi_client.setInsecure();
      if (_cfg.ca_fp[0]) {
        Serial.printf("[MQTT] WARN: ca_fp set in config but fingerprint API removed in ESP32 SDK >=2.\n"
                      "[MQTT]       Upload PEM cert to /mqtt_ca.pem for cert validation. TLS active, no validation.\n");
      } else {
        Serial.println("[MQTT] WARN: no /mqtt_ca.pem — TLS active but cert not validated");
      }
    }
  } else {
    // Non-TLS: only acceptable for loopback (127.0.0.1)
    bool is_loopback = (strncmp(_cfg.host, "127.", 4) == 0 ||
                        strcmp(_cfg.host, "localhost") == 0);
    if (!is_loopback) {
      strncpy(_last_error, "TLS required for non-loopback", sizeof(_last_error) - 1);
      Serial.println("[MQTT] ERROR: TLS=off only allowed for loopback. Enable TLS.");
      return false;
    }
  }

  _mqtt.setServer(_cfg.host, _cfg.port);
  _mqtt.setBufferSize(512);  // post envelopes can reach ~450 bytes

  // Build client ID (fall back to node name if not configured)
  char cid[72];
  if (_cfg.client_id[0]) {
    strncpy(cid, _cfg.client_id, sizeof(cid) - 1);
  } else {
    snprintf(cid, sizeof(cid), "siren-%s", _mesh.getNodeName());
  }
  cid[sizeof(cid) - 1] = 0;

  // LWT: offline status retained on broker when this client disconnects
  String lwt_topic = nodeTopic("status");

  const char* user = _cfg.user[0] ? _cfg.user : nullptr;
  const char* pw   = _cfg.pass[0] ? _cfg.pass : nullptr;  // never logged
  const char* lwt  = "{\"status\":\"offline\"}";

  bool ok = _mqtt.connect(cid, user, pw,
                          lwt_topic.c_str(), /*willQos=*/0, /*willRetain=*/true, lwt);
  if (!ok) {
    snprintf(_last_error, sizeof(_last_error), "connect failed rc=%d", _mqtt.state());
    Serial.printf("[MQTT] Connect failed: rc=%d host=%s:%d\n",
                  _mqtt.state(), _cfg.host, (int)_cfg.port);
    return false;
  }

  _last_error[0] = 0;
  Serial.printf("[MQTT] Connected to %s:%d as %s\n", _cfg.host, (int)_cfg.port, cid);

  // Publish online status + meta for all active rooms + initial VV
  publishStatus(true);
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (_mesh.isRoomActive(i)) publishMeta(i);
  }
  publishVV();
  _last_vv_publish = millis();

  return true;
}

/* ------------------------------------------------------------------ */
/*  Public interface                                                    */
/* ------------------------------------------------------------------ */
void MqttManager::begin() {
  loadConfig();
  _started = true;
  if (_cfg.enable) {
    Serial.printf("[MQTT] enabled — broker %s:%d TLS=%d\n",
                  _cfg.host, (int)_cfg.port, (int)_cfg.tls);
  } else {
    Serial.println("[MQTT] disabled (enable with: mqtt enable)");
  }
}

void MqttManager::setOtaSuspend(bool s) {
  _ota_suspend = s;
  if (s) {
    // Drop the broker connection now so WiFiClientSecure frees its TLS heap
    // (~40 KB) before the OTA HTTPS download opens its own TLS context.
    if (_mqtt.connected()) _mqtt.disconnect();
    _wifi_client.stop();
    Serial.println("[MQTT] Suspended for OTA (TLS freed)");
  } else {
    _last_connect_attempt = 0;  // reconnect promptly once OTA is done/aborted
    Serial.println("[MQTT] Resumed after OTA");
  }
}

void MqttManager::loop() {
  if (!_cfg.enable) return;

  // JES-876: while an OTA download is in progress the broker stays disconnected
  // so its TLS heap is available to the OTA HTTPS client.
  if (_ota_suspend) {
    if (_mqtt.connected()) _mqtt.disconnect();
    return;
  }

  // Guard: skip MQTT entirely when WiFi is not up (JES-864).
  // connectBroker() makes a blocking TLS call that can hang the loop for
  // seconds and trigger the Task WDT when the broker is unreachable.
  if (WiFi.status() != WL_CONNECTED) {
    _last_connect_attempt = 0;  // retry immediately once WiFi is back
    return;
  }

  unsigned long now = millis();

  // Non-blocking reconnect (millis-driven, no blocking delay)
  if (!_mqtt.connected()) {
    if (now - _last_connect_attempt >= RECONNECT_INTERVAL_MS) {
      _last_connect_attempt = now;
      connectBroker();
    }
    return;
  }

  _mqtt.loop();

  // Heartbeat VV publish
  uint32_t interval_ms = (uint32_t)_cfg.pub_interval_sec * 1000UL;
  if (now - _last_vv_publish >= interval_ms) {
    _last_vv_publish = now;
    publishVV();
  }
}

void MqttManager::onPostAdded(int room_idx, uint32_t timestamp,
                               const uint8_t* author_pub, const char* text) {
  if (!_cfg.enable || !_mqtt.connected()) return;

  String env = buildPostEnvelope(room_idx, timestamp, author_pub, text);
  if (env.isEmpty()) return;

  String topic = roomTopic(room_idx, "msg");
  if (topic.isEmpty()) return;

  bool ok = _mqtt.publish(topic.c_str(), env.c_str(), /*retained=*/false);
  if (!ok) {
    Serial.printf("[MQTT] publish failed (topic=%s len=%d)\n",
                  topic.c_str(), (int)env.length());
  }
}

/* ------------------------------------------------------------------ */
/*  CLI handler                                                         */
/* ------------------------------------------------------------------ */
bool MqttManager::handleMqttCommand(const char* args, char* reply) {
  reply[0] = 0;

  if (strncmp(args, "status", 6) == 0) {
    snprintf(reply, 160,
             "MQTT: %s | connected: %s | broker: %s:%d | TLS: %s | net: %s",
             _cfg.enable ? "ENABLED" : "DISABLED",
             _mqtt.connected() ? "YES" : "NO",
             _cfg.host[0] ? _cfg.host : "(none)",
             (int)_cfg.port,
             _cfg.tls ? "on" : "off",
             _cfg.net_id);
    if (_last_error[0]) {
      int n = strlen(reply);
      snprintf(reply + n, 160 - n, " | err: %s", _last_error);
    }
    return true;
  }

  if (strcmp(args, "enable") == 0) {
    _cfg.enable = true;
    saveConfig();
    _last_connect_attempt = 0;  // trigger immediate reconnect attempt
    strncpy(reply, "MQTT enabled", 159);
    return true;
  }

  if (strcmp(args, "disable") == 0) {
    _cfg.enable = false;
    if (_mqtt.connected()) _mqtt.disconnect();
    saveConfig();
    strncpy(reply, "MQTT disabled", 159);
    return true;
  }

  if (strncmp(args, "set ", 4) == 0) {
    const char* rest = args + 4;

    auto setField = [&](const char* key, char* field, size_t field_len, const char* prefix) -> bool {
      size_t plen = strlen(prefix);
      if (strncmp(rest, prefix, plen) == 0 && rest[plen] == ' ') {
        strncpy(field, rest + plen + 1, field_len - 1);
        field[field_len - 1] = 0;
        saveConfig();
        snprintf(reply, 160, "%s set", key);
        return true;
      }
      return false;
    };

    if (setField("host",      _cfg.host,      sizeof(_cfg.host),      "host"))      return true;
    if (setField("ca_fp",     _cfg.ca_fp,      sizeof(_cfg.ca_fp),    "ca_fp"))     return true;
    if (setField("user",      _cfg.user,       sizeof(_cfg.user),     "user"))      return true;
    if (setField("client_id", _cfg.client_id,  sizeof(_cfg.client_id),"client_id")) return true;
    if (setField("net_id",    _cfg.net_id,     sizeof(_cfg.net_id),   "net_id"))    return true;

    // pass: set but never echo
    if (strncmp(rest, "pass ", 5) == 0) {
      strncpy(_cfg.pass, rest + 5, sizeof(_cfg.pass) - 1);
      _cfg.pass[sizeof(_cfg.pass) - 1] = 0;
      saveConfig();
      strncpy(reply, "password set (write-only)", 159);
      return true;
    }

    if (strncmp(rest, "port ", 5) == 0) {
      int p = atoi(rest + 5);
      if (p < 1 || p > 65535) { strncpy(reply, "port must be 1-65535", 159); return true; }
      _cfg.port = (uint16_t)p;
      saveConfig();
      snprintf(reply, 160, "port set to %d", p);
      return true;
    }

    if (strncmp(rest, "tls ", 4) == 0) {
      _cfg.tls = (strcmp(rest + 4, "on") == 0);
      saveConfig();
      snprintf(reply, 160, "TLS %s", _cfg.tls ? "on" : "off");
      return true;
    }

    if (strncmp(rest, "qos ", 4) == 0) {
      int q = atoi(rest + 4);
      if (q < 0 || q > 1) { strncpy(reply, "qos must be 0 or 1", 159); return true; }
      _cfg.qos = (uint8_t)q;
      saveConfig();
      snprintf(reply, 160, "QoS set to %d", q);
      return true;
    }

    if (strncmp(rest, "interval ", 9) == 0) {
      int v = atoi(rest + 9);
      if (v < 30 || v > 3600) { strncpy(reply, "interval must be 30-3600 s", 159); return true; }
      _cfg.pub_interval_sec = (uint16_t)v;
      saveConfig();
      snprintf(reply, 160, "VV publish interval set to %d s", v);
      return true;
    }

    strncpy(reply, "unknown mqtt set key", 159);
    return true;
  }

  strncpy(reply, "usage: mqtt status|enable|disable|set <key> <val>", 159);
  return true;
}

/* ------------------------------------------------------------------ */
/*  Web section builder                                                 */
/* ------------------------------------------------------------------ */
String MqttManager::buildWebSection() {
  String page = "<div class='card'><h2>MQTT</h2>";

  page += "<p>Status: ";
  if (!_cfg.enable) {
    page += "<b class='warn'>DISABLED</b>";
  } else if (_mqtt.connected()) {
    page += "<b class='ok'>CONNECTED</b> to ";
    page += _cfg.host[0] ? _cfg.host : "(not set)";
    page += ":";
    page += (int)_cfg.port;
  } else {
    page += "<b class='err'>DISCONNECTED</b>";
    if (_last_error[0]) {
      page += " &mdash; ";
      page += _last_error;
    }
  }
  page += "</p>";

  // Enable/disable toggle
  page += "<form method='post' action='/api/mqtt/toggle' style='display:inline'>"
          "<button name='enable' value='";
  page += _cfg.enable ? "off" : "on";
  page += "'>";
  page += _cfg.enable ? "Disable MQTT" : "Enable MQTT";
  page += "</button></form> ";

  // Config form
  page += "<form method='post' action='/api/mqtt/set' style='margin-top:8px'>"
          "<b>Broker:</b> <input name='host' value='";
  page += _cfg.host;
  page += "' size='30' placeholder='mqtt.example.com'> "
          "Port: <input name='port' type='number' value='";
  page += (int)_cfg.port;
  page += "' size='6' min='1' max='65535'> "
          "TLS: <select name='tls'>"
          "<option value='on'";
  page += _cfg.tls ? " selected" : "";
  page += ">ON (recommended)</option>"
          "<option value='off'";
  page += !_cfg.tls ? " selected" : "";
  page += ">OFF (loopback only)</option>"
          "</select><br>"
          "CA Fingerprint (SHA-1): <input name='ca_fp' value='";
  page += _cfg.ca_fp;
  page += "' size='60' placeholder='AA:BB:CC:... (optional, leave blank to skip cert check)'><br>"
          "User: <input name='user' value='";
  page += _cfg.user;
  page += "' size='24'> "
          "Password: <input name='pass' type='password' size='24' "
          "placeholder='[set new password or leave blank to keep]'><br>"
          "Client ID: <input name='client_id' value='";
  page += _cfg.client_id;
  page += "' size='24' placeholder='auto'> "
          "Network ID: <input name='net_id' value='";
  page += _cfg.net_id;
  page += "' size='16'> "
          "QoS: <select name='qos'>"
          "<option value='0'";
  page += (_cfg.qos == 0) ? " selected" : "";
  page += ">0</option><option value='1'";
  page += (_cfg.qos == 1) ? " selected" : "";
  page += ">1</option></select> "
          "VV interval: <input name='interval' type='number' value='";
  page += (int)_cfg.pub_interval_sec;
  page += "' size='6' min='30' max='3600'> s "
          "<button type='submit'>Save MQTT</button></form>";

  page += "<p style='font-size:0.85em;color:#aaa'>"
          "Post payloads are AES-256-CTR encrypted (key derived from room identity). "
          "Broker sees only ciphertext. MQTT credentials are never exported in backup. "
          "CLI: <code>mqtt status | enable | disable | set &lt;key&gt; &lt;val&gt;</code></p>";
  page += "</div>";
  return page;
}

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
