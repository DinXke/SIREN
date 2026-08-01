#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include "WebManager.h"
#include "MqttManager.h"
#include <AsyncElegantOTA.h>
#include <qrcode.h>

// Default AP SSID prefix — node name is appended at runtime
#define AP_SSID_PREFIX "SIREN-"

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------
WebManager::WebManager(MultiRoomMesh& mesh)
  : _server(80), _mesh(mesh), _ui_task(nullptr), _mqtt_mgr(nullptr),
    _started(false), _dns_started(false),
    _mode(MODE_AP),
    _connect_started(0), _connecting(false),
    _ntp_synced(false), _ntp_check_ms(0)
{
  // Default AP SSID: "SIREN-Node" — overwritten in begin() with real node name
  strncpy(_ap_ssid, "SIREN-Node", sizeof(_ap_ssid) - 1);
  _ap_ssid[sizeof(_ap_ssid) - 1] = 0;
  _ap_pass[0]  = 0;
  _sta_ssid[0] = 0;
  _sta_pass[0] = 0;
  strncpy(_ntp_server, "pool.ntp.org", sizeof(_ntp_server) - 1);
  _ntp_server[sizeof(_ntp_server) - 1] = 0;
}

// ---------------------------------------------------------------------------
//  Config persistence (hand-rolled JSON — avoids ArduinoJson dependency)
// ---------------------------------------------------------------------------
void WebManager::loadConfig() {
  File f = SPIFFS.open(WIFI_CONFIG_PATH, "r");
  if (!f) return;

  String raw = f.readString();
  f.close();

  auto extractField = [&](const char* key, char* dest, size_t dest_len) {
    String k = String("\"") + key + "\":\"";
    int start = raw.indexOf(k);
    if (start < 0) return;
    start += k.length();
    int end = raw.indexOf("\"", start);
    if (end < 0) return;
    String val = raw.substring(start, end);
    strncpy(dest, val.c_str(), dest_len - 1);
    dest[dest_len - 1] = 0;
  };

  // Mode
  char mode_str[8] = {};
  extractField("mode", mode_str, sizeof(mode_str));
  _mode = (strcmp(mode_str, "sta") == 0) ? MODE_STA : MODE_AP;

  extractField("ap_ssid",    _ap_ssid,    sizeof(_ap_ssid));
  extractField("ap_pass",    _ap_pass,    sizeof(_ap_pass));
  extractField("sta_ssid",   _sta_ssid,   sizeof(_sta_ssid));
  extractField("sta_pass",   _sta_pass,   sizeof(_sta_pass));
  // ntp_server is optional — keep default "pool.ntp.org" if absent
  char ntp_buf[64] = {};
  extractField("ntp_server", ntp_buf, sizeof(ntp_buf));
  if (ntp_buf[0]) {
    strncpy(_ntp_server, ntp_buf, sizeof(_ntp_server) - 1);
    _ntp_server[sizeof(_ntp_server) - 1] = 0;
  }
}

void WebManager::saveConfig() {
  File f = SPIFFS.open(WIFI_CONFIG_PATH, "w");
  if (!f) return;
  f.printf("{\"mode\":\"%s\","
           "\"ap_ssid\":\"%s\",\"ap_pass\":\"%s\","
           "\"sta_ssid\":\"%s\",\"sta_pass\":\"%s\","
           "\"ntp_server\":\"%s\"}",
           _mode == MODE_STA ? "sta" : "ap",
           _ap_ssid, _ap_pass,
           _sta_ssid, _sta_pass,
           _ntp_server);
  f.close();
}

// ---------------------------------------------------------------------------
//  Clock epoch persistence — keeps time reasonable across power cycles even
//  when NTP is unavailable (e.g. AP-only mode).  A single text file stores
//  the last RTC epoch; it is restored on boot and updated after every
//  authoritative sync (NTP or browser).
// ---------------------------------------------------------------------------
#define CLOCK_EPOCH_PATH  "/clock_epoch"
// Minimum plausible epoch: 2026-01-01 00:00:00 UTC
#define CLOCK_EPOCH_FLOOR 1767225600UL

void WebManager::loadClockEpoch() {
  File f = SPIFFS.open(CLOCK_EPOCH_PATH, "r");
  if (!f) return;
  String s = f.readStringUntil('\n');
  f.close();
  uint32_t saved = (uint32_t)s.toInt();
  if (saved > CLOCK_EPOCH_FLOOR) {
    uint32_t curr = (uint32_t)_mesh.getRTCClock()->getCurrentTime();
    if (saved > curr) {
      _mesh.getRTCClock()->setCurrentTime(saved);
      Serial.printf("[Clock] Restored saved epoch %lu from SPIFFS\n",
                    (unsigned long)saved);
    }
  }
}

void WebManager::saveClockEpoch() {
  uint32_t now = (uint32_t)_mesh.getRTCClock()->getCurrentTime();
  if (now <= CLOCK_EPOCH_FLOOR) return;  // don't persist obviously wrong time
  File f = SPIFFS.open(CLOCK_EPOCH_PATH, "w");
  if (!f) return;
  f.println((unsigned long)now);
  f.close();
}

// ---------------------------------------------------------------------------
//  WiFi startup helpers
// ---------------------------------------------------------------------------
void WebManager::startAP() {
  Serial.printf("[WiFi] Starting AP '%s' (%s)...\n",
                _ap_ssid, _ap_pass[0] ? "WPA2-PSK" : "open");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(_ap_ssid, _ap_pass[0] ? _ap_pass : nullptr);
  delay(100);  // allow AP interface to come up before binding server
  Serial.printf("[WiFi] AP ready — IP: %s\n",
                WiFi.softAPIP().toString().c_str());

  // Captive-portal DNS: redirect every hostname to this AP's IP
  if (!_dns_started) {
    _dns.start(53, "*", WiFi.softAPIP());
    _dns_started = true;
    Serial.println("[WiFi] Captive portal DNS started");
  }

  if (!_started) {
    setupRoutes();
    _started = true;
    Serial.printf("[WiFi] Web UI: http://%s/\n",
                  WiFi.softAPIP().toString().c_str());
  }
}

void WebManager::stopCaptivePortal() {
  if (_dns_started) {
    _dns.stop();
    _dns_started = false;
  }
}

void WebManager::connectSTA() {
  if (_sta_ssid[0] == 0) return;
  Serial.printf("[WiFi] Connecting to '%s'...\n", _sta_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(_sta_ssid, _sta_pass[0] ? _sta_pass : nullptr);
  _connect_started = millis();
  _connecting      = true;
}

// ---------------------------------------------------------------------------
//  Hex encode helpers
// ---------------------------------------------------------------------------
static void bytesToHex(const uint8_t* src, size_t len, char* out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2]     = hex[src[i] >> 4];
    out[i * 2 + 1] = hex[src[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
  return 0;
}

static size_t hexToBytes(const char* hex, uint8_t* out, size_t max_out) {
  size_t len = strlen(hex);
  if (len % 2 != 0) return 0;
  size_t n = len / 2;
  if (n > max_out) n = max_out;
  for (size_t i = 0; i < n; i++) {
    out[i] = (hexNibble(hex[i * 2]) << 4) | hexNibble(hex[i * 2 + 1]);
  }
  return n;
}

// Escape a C-string for JSON (replace \ and " only; control chars not expected)
static String jsonEscape(const char* s) {
  String r;
  for (; *s; s++) {
    if (*s == '"')       r += "\\\"";
    else if (*s == '\\') r += "\\\\";
    else                 r += *s;
  }
  return r;
}

// Escape a C-string for safe HTML display (XSS prevention)
static String htmlEscape(const char* s) {
  String r;
  for (; *s; s++) {
    if      (*s == '&')  r += "&amp;";
    else if (*s == '<')  r += "&lt;";
    else if (*s == '>')  r += "&gt;";
    else if (*s == '"')  r += "&quot;";
    else                 r += *s;
  }
  return r;
}

// CSRF mitigation: verify the Origin header matches our host.
// Modern browsers always include Origin on cross-origin form POSTs.
// If Origin is absent (curl, direct navigation) we allow through.
// Returns false (= reject) only when Origin is present and does NOT match.
static bool checkOrigin(AsyncWebServerRequest* req) {
  String origin = req->header("Origin");
  if (origin.length() == 0) return true;
  String host = req->host();
  return (origin == "http://" + host || origin == "https://" + host);
}

// ---------------------------------------------------------------------------
//  Backup JSON builder
// ---------------------------------------------------------------------------
String WebManager::buildBackupJson() {
  const NodePrefs* p = _mesh.getNodePrefs();

  String j = "{";
  j += "\"version\":\"2\",";
  j += "\"node_name\":\"" + jsonEscape(p->node_name) + "\",";
  j += "\"admin_pass\":\"" + jsonEscape(p->password)  + "\",";

  // Radio
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%.3f", (double)p->freq);
  j += "\"freq\":\"";   j += tmp; j += "\",";
  snprintf(tmp, sizeof(tmp), "%.1f", (double)p->bw);
  j += "\"bw\":\"";     j += tmp; j += "\",";
  snprintf(tmp, sizeof(tmp), "%d",   (int)p->sf);
  j += "\"sf\":\"";     j += tmp; j += "\",";
  snprintf(tmp, sizeof(tmp), "%d",   (int)p->cr);
  j += "\"cr\":\"";     j += tmp; j += "\",";
  snprintf(tmp, sizeof(tmp), "%d",   (int)p->tx_power_dbm);
  j += "\"tx_power\":\""; j += tmp; j += "\",";

  // WiFi
  j += "\"wifi_mode\":\"";  j += (_mode == MODE_STA ? "sta" : "ap"); j += "\",";
  j += "\"ap_ssid\":\"" + jsonEscape(_ap_ssid) + "\",";
  j += "\"ap_pass\":\"" + jsonEscape(_ap_pass)  + "\",";
  j += "\"sta_ssid\":\"" + jsonEscape(_sta_ssid) + "\",";
  j += "\"sta_pass\":\"" + jsonEscape(_sta_pass)  + "\",";
  j += "\"ntp_server\":\"" + jsonEscape(_ntp_server) + "\",";

  // Rooms — identity bytes = prv(64) || pub(32) = 96 bytes = 192 hex chars
  // PRV_KEY_SIZE=64, PUB_KEY_SIZE=32
  uint8_t id_buf[96];
  char    id_hex[193];
  for (int i = 0; i < MAX_ROOMS; i++) {
    snprintf(tmp, sizeof(tmp), "%d", i);
    j += "\"room"; j += tmp; j += "_active\":\"";
    j += (_mesh.isRoomActive(i) ? "1" : "0"); j += "\",";
    j += "\"room"; j += tmp; j += "_name\":\"";
    j += jsonEscape(_mesh.getRoomName(i)); j += "\",";
    j += "\"room"; j += tmp; j += "_pass\":\"";
    j += jsonEscape(_mesh.getRoomPassword(i)); j += "\",";
    j += "\"room"; j += tmp; j += "_guest\":\"";
    j += jsonEscape(_mesh.getRoomGuestPassword(i)); j += "\",";

    // Identity bytes (only for active rooms; inactive = all-zero placeholder)
    memset(id_buf, 0, sizeof(id_buf));
    if (_mesh.isRoomActive(i)) {
      _mesh.getRoomIdentityBytes(i, id_buf, sizeof(id_buf));
    }
    bytesToHex(id_buf, sizeof(id_buf), id_hex);
    j += "\"room"; j += tmp; j += "_id\":\"";
    j += id_hex; j += "\"";
    if (i < MAX_ROOMS - 1) j += ",";
  }

  // Peers (JES-816): each active peer as peer<n>_pub (64 hex) + peer<n>_name
  for (int i = 0; i < MAX_PEERS; i++) {
    const PeerInfo* peer = _mesh.getPeer(i);
    if (!peer || !peer->active) continue;
    char peer_pub_hex[65] = {};
    for (int b = 0; b < PUB_KEY_SIZE; b++)
      snprintf(peer_pub_hex + b * 2, 3, "%02x", (unsigned int)peer->pub_key[b]);
    snprintf(tmp, sizeof(tmp), "%d", i);
    j += ",\"peer"; j += tmp; j += "_pub\":\"";  j += peer_pub_hex;    j += "\"";
    j += ",\"peer"; j += tmp; j += "_name\":\""; j += jsonEscape(peer->name); j += "\"";
  }

  // Post pool (JES-790): flat key-value pairs for all active posts
  j += ",";
  j += _mesh.getPostsFlatJson();

  // Name table (JES-798): hex-encode raw SPIFFS /names file
  {
    File nf = SPIFFS.open("/names", "r");
    if (nf) {
      size_t sz = nf.size();
      j += ",\"names_hex\":\"";
      while (sz-- > 0) {
        uint8_t b = nf.read();
        static const char hx[] = "0123456789abcdef";
        j += hx[b >> 4];
        j += hx[b & 0x0f];
      }
      nf.close();
      j += "\"";
    }
  }

  j += "}";
  return j;
}

// ---------------------------------------------------------------------------
//  Restore: parse flat JSON and apply settings
// ---------------------------------------------------------------------------
bool WebManager::applyRestore(const String& json) {
  if (json.length() < 10) return false;

  // Simple field extractor (same approach as loadConfig)
  auto extractField = [&](const char* key, char* dest, size_t dest_len) -> bool {
    String k = String("\"") + key + "\":\"";
    int start = json.indexOf(k);
    if (start < 0) return false;
    start += k.length();
    int end = json.indexOf("\"", start);
    if (end < 0) return false;
    String val = json.substring(start, end);
    // Unescape \" and \\
    val.replace("\\\"", "\"");
    val.replace("\\\\", "\\");
    strncpy(dest, val.c_str(), dest_len - 1);
    dest[dest_len - 1] = 0;
    return true;
  };

  // Validate version
  char ver[4] = {};
  if (!extractField("version", ver, sizeof(ver)) ||
      (strcmp(ver, "1") != 0 && strcmp(ver, "2") != 0)) return false;

  char reply[160];
  char val[128];

  // Node name
  if (extractField("node_name", val, sizeof(val)) && val[0]) {
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "set name %s", val);
    _mesh.handleCommand(0, cmd, reply);
  }

  // Admin password
  if (extractField("admin_pass", val, sizeof(val)) && val[0]) {
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "password %s", val);
    _mesh.handleCommand(0, cmd, reply);
  }

  // Radio settings
  if (extractField("freq", val, sizeof(val)))     { char c[64]; snprintf(c, sizeof(c), "freq %s", val);     _mesh.handleCommand(0, c, reply); }
  if (extractField("sf",   val, sizeof(val)))     { char c[64]; snprintf(c, sizeof(c), "sf %s", val);       _mesh.handleCommand(0, c, reply); }
  if (extractField("bw",   val, sizeof(val)))     { char c[64]; snprintf(c, sizeof(c), "bw %s", val);       _mesh.handleCommand(0, c, reply); }
  if (extractField("cr",   val, sizeof(val)))     { char c[64]; snprintf(c, sizeof(c), "cr %s", val);       _mesh.handleCommand(0, c, reply); }
  if (extractField("tx_power", val, sizeof(val))) { char c[64]; snprintf(c, sizeof(c), "txpow %s", val);    _mesh.handleCommand(0, c, reply); }

  // WiFi settings
  char wifi_mode[8]   = {};
  char ap_ssid[64]    = {};
  char ap_pass[64]    = {};
  char sta_ssid[64]   = {};
  char sta_pass[64]   = {};
  extractField("wifi_mode", wifi_mode, sizeof(wifi_mode));
  extractField("ap_ssid",   ap_ssid,   sizeof(ap_ssid));
  extractField("ap_pass",   ap_pass,   sizeof(ap_pass));
  extractField("sta_ssid",  sta_ssid,  sizeof(sta_ssid));
  extractField("sta_pass",  sta_pass,  sizeof(sta_pass));

  _mode = (strcmp(wifi_mode, "sta") == 0) ? MODE_STA : MODE_AP;
  if (ap_ssid[0]) strncpy(_ap_ssid, ap_ssid, sizeof(_ap_ssid) - 1);
  strncpy(_ap_pass,  ap_pass,  sizeof(_ap_pass)  - 1);
  if (sta_ssid[0]) strncpy(_sta_ssid, sta_ssid, sizeof(_sta_ssid) - 1);
  strncpy(_sta_pass, sta_pass, sizeof(_sta_pass) - 1);
  char ntp_server_r[64] = {};
  extractField("ntp_server", ntp_server_r, sizeof(ntp_server_r));
  if (ntp_server_r[0]) {
    strncpy(_ntp_server, ntp_server_r, sizeof(_ntp_server) - 1);
    _ntp_server[sizeof(_ntp_server) - 1] = 0;
  }
  saveConfig();

  // Rooms — 96 bytes identity (prv64 + pub32) hex-encoded
  for (int i = 0; i < MAX_ROOMS; i++) {
    char active_key[20], name_key[20], pass_key[20], guest_key[20], id_key[20];
    snprintf(active_key, sizeof(active_key), "room%d_active", i);
    snprintf(name_key,   sizeof(name_key),   "room%d_name",   i);
    snprintf(pass_key,   sizeof(pass_key),   "room%d_pass",   i);
    snprintf(guest_key,  sizeof(guest_key),  "room%d_guest",  i);
    snprintf(id_key,     sizeof(id_key),     "room%d_id",     i);

    char active_str[4]  = {};
    char room_name[24]  = {};
    char room_pass[16]  = {};
    char room_guest[16] = {};
    char room_id[193]   = {};

    extractField(active_key, active_str, sizeof(active_str));
    bool should_be_active = (strcmp(active_str, "1") == 0);

    if (!should_be_active) {
      if (i > 0) _mesh.deactivateRoom(i);  // room 0 always stays active
      continue;
    }

    extractField(name_key,  room_name,  sizeof(room_name));
    extractField(pass_key,  room_pass,  sizeof(room_pass));
    extractField(guest_key, room_guest, sizeof(room_guest));
    _mesh.activateRoom(i, room_name[0] ? room_name : nullptr, room_pass, room_guest);

    if (extractField(id_key, room_id, sizeof(room_id)) && strlen(room_id) == 192) {
      uint8_t id_buf[96];
      size_t  n = hexToBytes(room_id, id_buf, sizeof(id_buf));
      if (n == 96) {
        _mesh.setRoomIdentityFromBytes(i, id_buf, n);
      }
    }
  }

  // Restore peers (JES-816) — present in v2 backups that include them
  {
    char peer_pub_key[20], peer_name_key[20];
    char peer_pub_hex[65] = {}, peer_name[24] = {};
    for (int i = 0; i < MAX_PEERS; i++) {
      snprintf(peer_pub_key,  sizeof(peer_pub_key),  "peer%d_pub",  i);
      snprintf(peer_name_key, sizeof(peer_name_key), "peer%d_name", i);
      if (!extractField(peer_pub_key, peer_pub_hex, sizeof(peer_pub_hex))) continue;
      if (strlen(peer_pub_hex) != 64) continue;
      // Validate hex charset
      bool valid = true;
      for (int c = 0; c < 64; c++) {
        char ch = peer_pub_hex[c];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
          valid = false; break;
        }
      }
      if (!valid) continue;
      uint8_t key[PUB_KEY_SIZE] = {};
      if (!mesh::Utils::fromHex(key, PUB_KEY_SIZE, peer_pub_hex)) continue;
      extractField(peer_name_key, peer_name, sizeof(peer_name));
      _mesh.addPeerFromWeb(key, peer_name[0] ? peer_name : nullptr);
    }
  }

  // Restore post pool for version 2 backups (JES-790)
  if (strcmp(ver, "2") == 0) {
    _mesh.restorePostsFlatJson(json);
  }

  // Restore name table hex blob (JES-798) — present in v2 backups that include it
  {
    String k = String("\"names_hex\":\"");
    int pos = json.indexOf(k);
    if (pos >= 0) {
      pos += k.length();
      int end_pos = json.indexOf("\"", pos);
      if (end_pos > pos) {
        File nf = SPIFFS.open("/names", "w");
        if (nf) {
          for (int i = pos; i + 1 < end_pos; i += 2) {
            uint8_t b = (hexNibble(json[i]) << 4) | hexNibble(json[i + 1]);
            nf.write(b);
          }
          nf.close();
        }
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
//  Base64 encoder (used for QR module bitmap)
// ---------------------------------------------------------------------------
static String base64Encode(const uint8_t* data, size_t len) {
  static const char enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t b = ((uint32_t)data[i] << 16)
               | (i + 1 < len ? (uint32_t)data[i + 1] << 8 : 0)
               | (i + 2 < len ? (uint32_t)data[i + 2]      : 0);
    out += enc[(b >> 18) & 63];
    out += enc[(b >> 12) & 63];
    out += (i + 1 < len ? enc[(b >>  6) & 63] : '=');
    out += (i + 2 < len ? enc[ b        & 63] : '=');
  }
  return out;
}

// ---------------------------------------------------------------------------
//  PrintSink — forwards String-like += calls to an AsyncResponseStream.
//  Avoids allocating a single large contiguous heap block for the full page.
//  AsyncResponseStream internally uses a linked list of 1460-byte chunks,
//  so even a 40KB+ page never needs more than ~2KB of contiguous heap. (JES-854)
// ---------------------------------------------------------------------------
struct PrintSink {
  Print& _p;
  explicit PrintSink(Print& p) : _p(p) {}
  void reserve(size_t) {}                              // no-op
  PrintSink& operator+=(const char* s)                { if (s) _p.print(s); return *this; }
  PrintSink& operator+=(const String& s)              { _p.print(s); return *this; }
  PrintSink& operator+=(const __FlashStringHelper* f) { _p.print(f); return *this; }
  PrintSink& operator+=(int i)           { _p.print(i); return *this; }
  PrintSink& operator+=(unsigned int u)  { _p.print(u); return *this; }
  PrintSink& operator+=(long l)          { _p.print(l); return *this; }
  PrintSink& operator+=(unsigned long u) { _p.print(u); return *this; }
  PrintSink& operator+=(uint8_t u)       { _p.print(u); return *this; }
  PrintSink& operator+=(char c)          { _p.print(c); return *this; }
};

// ---------------------------------------------------------------------------
//  HTML page builders
// ---------------------------------------------------------------------------
static const char HTML_HEAD_PRE[] PROGMEM =
  "<!DOCTYPE html><html><head>"
  "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>"
  "<meta charset='utf-8'>"
  "<title>";

static const char HTML_HEAD_POST[] PROGMEM =
  "</title>"
  "<style>"
  /* === Reset & Base === */
  "*{box-sizing:border-box}"
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
       "background:#0f1117;color:#e0e0e0;margin:0;padding:0;font-size:16px;line-height:1.4}"
  "h2{color:#00d4ff;font-size:1.1em;margin:0 0 10px;font-weight:600}"
  "h3{color:#00d4ff;font-size:1em;margin:0 0 8px;font-weight:600}"
  "a{color:#00d4ff}"
  "code{font-family:monospace;font-size:0.85em;background:#1e2235;padding:1px 4px;border-radius:3px}"
  /* === Cards === */
  ".card{background:#16213e;border:1px solid #2a3050;padding:14px;margin-bottom:12px;"
        "border-radius:8px;overflow-x:auto}"
  /* === Tables — scrollable on small screens === */
  "table{border-collapse:collapse;width:100%;margin-bottom:8px;font-size:0.88em}"
  "th,td{border:1px solid #2a3050;padding:6px 10px;text-align:left}"
  "th{background:#1e2a4a;color:#00d4ff;white-space:nowrap}"
  /* === Forms — full-width on mobile === */
  "form{margin-top:10px}"
  ".frow{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:6px}"
  ".frow label{color:#aaa;font-size:0.88em;min-width:80px}"
  "input,select{background:#1e2235;border:1px solid #3a4060;color:#e0e0e0;"
               "padding:10px 12px;border-radius:6px;font-size:0.95em;"
               "width:100%;max-width:400px;min-height:44px}"
  "input[type=number]{max-width:110px}"
  "input[type=file]{padding:8px;min-height:44px}"
  "select{min-height:44px}"
  /* === Buttons === */
  "button,.btn{background:#00d4ff;border:none;color:#000;padding:10px 18px;"
              "border-radius:6px;cursor:pointer;font-size:0.95em;font-weight:600;"
              "min-height:44px;min-width:60px;white-space:nowrap;margin:2px 0}"
  "button.sec{background:#2a3a4a;color:#e0e0e0;border:1px solid #3a4060}"
  "button.del{background:#b22222;color:#fff}"
  "button.warn-btn{background:#cc7700;color:#fff}"
  /* === Status indicators === */
  ".ok{color:#00ff88}.err{color:#ff4444}.warn{color:#ffcc00}"
  /* === Top nav bar === */
  "#topbar{background:#0d1020;border-bottom:1px solid #2a3050;"
           "padding:10px 14px;display:flex;align-items:center;justify-content:space-between}"
  "#topbar h1{font-size:1em;color:#00d4ff;margin:0;font-weight:600}"
  "#topbar a{color:#aaa;font-size:0.85em;text-decoration:none}"
  /* === Tab navigation === */
  "#tabnav{display:flex;overflow-x:auto;background:#0d1020;border-bottom:1px solid #2a3050;"
           "padding:0 6px;-webkit-overflow-scrolling:touch;scrollbar-width:none}"
  "#tabnav::-webkit-scrollbar{display:none}"
  ".tnb{flex:none;padding:12px 16px;color:#aaa;cursor:pointer;border:none;"
        "background:transparent;font-size:0.9em;font-weight:500;border-bottom:2px solid transparent;"
        "white-space:nowrap;min-height:44px}"
  ".tnb.act{color:#00d4ff;border-bottom-color:#00d4ff}"
  /* === Tab panels === */
  ".tpanel{display:none;padding:12px 10px}"
  ".tpanel.act{display:block}"
  /* === Inline button groups in table cells === */
  ".btngrp{display:flex;flex-wrap:wrap;gap:4px}"
  ".btngrp form{margin:0}"
  ".btngrp button{padding:7px 12px;min-height:36px;font-size:0.82em}"
  /* === Mono hex display === */
  ".hex{font-family:monospace;font-size:0.78em;word-break:break-all;color:#aaa}"
  /* === Anchor nav links === */
  "a.tnb{display:flex;align-items:center;text-decoration:none}"
  /* === Responsive overrides for wider screens === */
  "@media(min-width:600px){"
    "body{padding:0}"
    ".tpanel{padding:16px 14px}"
    "input,select{width:auto}"
  "}"
  "</style></head><body>";

// Build the HTML <head> with the node name in the <title>.
static String buildHead(const char* node_name) {
  String h = FPSTR(HTML_HEAD_PRE);
  h += htmlEscape(node_name);
  h += F(" \u2014 SIREN");
  h += FPSTR(HTML_HEAD_POST);
  return h;
}

static const char HTML_FOOT[] PROGMEM = "</body></html>";

String WebManager::buildChatPage() {
  String page = buildHead(_mesh.getNodeName());

  // Tab CSS for chat room tabs (uses room-tab class to avoid conflict with top nav)
  page += "<style>"
          ".rtab{background:#1e2235;color:#aaa;border:1px solid #2a3050;"
                "padding:9px 16px;cursor:pointer;border-radius:6px;margin:2px;"
                "font-size:0.9em;min-height:40px;border:none}"
          ".rtab.act{background:#00d4ff;color:#000;font-weight:600}"
          "</style>";

  // Top bar with back link
  page += "<div id='topbar'>"
          "<h1>&#128172; Rooms &mdash; ";
  page += htmlEscape(_mesh.getNodeName());
  page += "</h1>"
          "<a href='/'>&#8592; Beheer</a></div>";

  // Tab bar — one tab per active room (skip room 0: identity-only, JES-846)
  int _firstRoom = -1;
  page += "<div style='display:flex;flex-wrap:wrap;gap:6px;padding:10px 10px 0'>";
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (!_mesh.isRoomActive(i)) continue;
    page += "<button class='rtab";
    page += (_firstRoom < 0 ? " act" : "");
    page += "' onclick='selTab(this,";
    page += i;
    page += ")'>";
    page += htmlEscape(_mesh.getRoomName(i));
    page += "</button>";
    if (_firstRoom < 0) _firstRoom = i;
  }
  page += "</div>";

  // Messages box — full width on mobile
  page += "<div style='padding:10px'>";
  page += "<div id='msgs' style='height:50vh;min-height:200px;max-height:420px;"
          "overflow-y:auto;background:#0d0d1a;border:1px solid #2a3050;"
          "padding:8px;font-size:0.9em;border-radius:6px'></div>";
  page += "<form id='post-form' onsubmit='postMsg(event)' style='margin-top:8px;display:flex;gap:6px'>"
          "<input id='post-txt' style='flex:1;min-width:0' maxlength='140' "
          "placeholder='Bericht als operator...'>"
          "<button type='submit' style='flex:none'>Post</button></form>";

  // Nicklist (collapsed by default on mobile, toggle button)
  page += "<div style='margin-top:8px'>"
          "<button class='sec' onclick='toggleNicks()' style='width:100%;text-align:left'>"
          "&#128100; Users <span id='nick-count'></span> &#9660;</button>"
          "<div id='nicks' style='display:none;background:#1e2235;border:1px solid #2a3050;"
          "border-radius:0 0 6px 6px;padding:8px;font-size:0.9em'></div>"
          "</div>";

  // DM pane (hidden until a user is clicked)
  page += "<div id='dm-pane' style='display:none;margin-top:8px;background:#16213e;"
          "border:1px solid #2a3050;border-radius:8px;padding:12px'>"
          "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px'>"
          "<b id='dm-title' style='color:#ffcc00'>DM</b>"
          "<button class='sec' onclick='closeDm()' style='min-height:36px;padding:4px 12px'>&#x2715; Sluiten</button>"
          "</div>"
          "<div id='dm-msgs' style='height:200px;overflow-y:auto;background:#0d0d1a;"
          "border:1px solid #2a3050;padding:8px;font-size:0.9em;border-radius:6px'></div>"
          "<form id='dm-form' onsubmit='sendDm(event)' style='margin-top:8px;display:flex;gap:6px'>"
          "<input id='dm-txt' style='flex:1;min-width:0' maxlength='140' placeholder='Privébericht...'>"
          "<button type='submit' style='flex:none'>Stuur</button></form>"
          "</div>";

  page += "</div>";  // padding wrapper

  // Inline JS — uses textContent (not innerHTML) for safe display
  page += "<script>\n";
  page += "var room="; page += (_firstRoom >= 0 ? _firstRoom : 0); page += ",since=0,pollT=null,nickT=null;\n";
  page += "var dmPub='',dmPollT=null;\n";
  page += "function selTab(btn,idx){\n"
          "  room=idx;since=0;\n"
          "  document.getElementById('msgs').innerHTML='';\n"
          "  var ts=document.querySelectorAll('.rtab');\n"
          "  for(var i=0;i<ts.length;i++)ts[i].classList.remove('act');\n"
          "  btn.classList.add('act');\n"
          "  clearTimeout(pollT);clearTimeout(nickT);\n"
          "  fetchMsgs();fetchNicks();\n"
          "}\n"
          "function toggleNicks(){\n"
          "  var n=document.getElementById('nicks');\n"
          "  n.style.display=(n.style.display==='none'?'block':'none');\n"
          "}\n"
          "function delPost(ridx,oid,pts,el){\n"
          "  if(!confirm('Verwijder dit bericht?'))return;\n"
          "  var body='room_idx='+encodeURIComponent(ridx)+'&origin_id='+encodeURIComponent(oid)+'&post_ts='+encodeURIComponent(pts);\n"
          "  fetch('/api/room/delpost',{method:'POST',credentials:'include',\n"
          "    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})\n"
          "  .then(function(r){\n"
          "    if(r.ok){el.parentNode&&el.parentNode.removeChild(el);}\n"
          "    else r.json().then(function(j){alert('Fout: '+(j.error||'onbekend'));});\n"
          "  }).catch(function(){alert('Verbindingsfout');});\n"
          "}\n"
          "function fetchMsgs(){\n"
          "  fetch('/api/chat/messages?room='+room+'&since='+since,{credentials:'include'})\n"
          "  .then(function(r){return r.json();})\n"
          "  .then(function(data){\n"
          "    var box=document.getElementById('msgs');\n"
          "    var atBottom=(box.scrollHeight-box.scrollTop-box.clientHeight<30);\n"
          "    data.forEach(function(m){\n"
          "      var row=document.createElement('div');\n"
          "      row.style.marginBottom='3px';\n"
          "      var ts=new Date(m.ts*1000).toLocaleTimeString();\n"
          "      var s1=document.createElement('span');\n"
          "      s1.style.color='#888';s1.style.fontSize='0.8em';\n"
          "      s1.textContent='['+ts+'] ';\n"
          "      var s2=document.createElement('span');\n"
          "      s2.style.color='#00d4ff';\n"
          "      s2.textContent='<'+m.author+'> ';\n"
          "      var s3=document.createTextNode(m.text);\n"
          "      var btn=document.createElement('button');\n"
          "      btn.textContent='[x]';\n"
          "      btn.style.cssText='margin-left:6px;font-size:0.7em;padding:1px 4px;cursor:pointer;background:transparent;color:#ff4444;border:1px solid #ff4444;border-radius:2px';\n"
          "      btn.title='Verwijder bericht';\n"
          "      (function(rr,oid,pts,el){btn.onclick=function(){delPost(rr,oid,pts,el);};})(room,m.origin_id,m.ts,row);\n"
          "      row.appendChild(s1);row.appendChild(s2);row.appendChild(s3);row.appendChild(btn);\n"
          "      box.appendChild(row);\n"
          "      if(m.ts>since)since=m.ts;\n"
          "    });\n"
          "    if(atBottom)box.scrollTop=box.scrollHeight;\n"
          "  }).catch(function(){});\n"
          "  pollT=setTimeout(fetchMsgs,4000);\n"
          "}\n"
          "function fetchNicks(){\n"
          "  fetch('/api/chat/nicks?room='+room,{credentials:'include'})\n"
          "  .then(function(r){return r.json();})\n"
          "  .then(function(data){\n"
          "    var box=document.getElementById('nicks');\n"
          "    var cnt=document.getElementById('nick-count');\n"
          "    if(cnt)cnt.textContent='('+data.length+')';\n"
          "    box.innerHTML='';\n"
          "    data.forEach(function(n){\n"
          "      var d=document.createElement('div');\n"
          "      d.style.padding='4px 0';\n"
          "      d.style.color=(n.role>=3?'#ffcc00':(n.role>=2?'#00ff88':'#aaa'));\n"
          "      d.style.cursor='pointer';\n"
          "      d.title='Klik om te DM\\'en';\n"
          "      d.textContent=n.name+(n.role>=3?' [op]':(n.role>=2?' [rw]':(n.role>=1?' [ro]':'')));\n"
          "      d.onclick=(function(pub,name){return function(){openDm(pub,name);};})(n.pub,n.name);\n"
          "      box.appendChild(d);\n"
          "    });\n"
          "  }).catch(function(){});\n"
          "  nickT=setTimeout(fetchNicks,5000);\n"
          "}\n"
          "function postMsg(e){\n"
          "  e.preventDefault();\n"
          "  var txt=document.getElementById('post-txt').value.trim();\n"
          "  if(!txt)return;\n"
          "  fetch('/api/chat/post',{method:'POST',credentials:'include',\n"
          "    headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
          "    body:'room='+encodeURIComponent(room)+'&text='+encodeURIComponent(txt)})\n"
          "  .then(function(r){if(r.ok)document.getElementById('post-txt').value='';});\n"
          "}\n"
          "function openDm(pub,name){\n"
          "  dmPub=pub;\n"
          "  document.getElementById('dm-title').textContent='DM: '+name;\n"
          "  document.getElementById('dm-pane').style.display='';\n"
          "  document.getElementById('dm-msgs').innerHTML='';\n"
          "  clearTimeout(dmPollT);\n"
          "  fetchDmThread();\n"
          "}\n"
          "function closeDm(){\n"
          "  clearTimeout(dmPollT);\n"
          "  dmPub='';\n"
          "  document.getElementById('dm-pane').style.display='none';\n"
          "}\n"
          "function fetchDmThread(){\n"
          "  if(!dmPub)return;\n"
          "  fetch('/api/dm/thread?pub='+dmPub,{credentials:'include'})\n"
          "  .then(function(r){return r.json();})\n"
          "  .then(function(data){\n"
          "    var box=document.getElementById('dm-msgs');\n"
          "    var atBottom=(box.scrollHeight-box.scrollTop-box.clientHeight<30);\n"
          "    box.innerHTML='';\n"
          "    data.forEach(function(m){\n"
          "      var row=document.createElement('div');\n"
          "      row.style.marginBottom='3px';\n"
          "      row.style.textAlign=(m.out?'right':'left');\n"
          "      var ts=new Date(m.ts*1000).toLocaleTimeString();\n"
          "      var s1=document.createElement('span');\n"
          "      s1.style.color='#888';s1.style.fontSize='0.75em';\n"
          "      s1.textContent='['+ts+'] ';\n"
          "      var s2=document.createElement('span');\n"
          "      s2.style.color=(m.out?'#ffcc00':'#00d4ff');\n"
          "      s2.textContent=(m.out?'[jij] ':'')+m.text;\n"
          "      row.appendChild(s1);row.appendChild(s2);\n"
          "      box.appendChild(row);\n"
          "    });\n"
          "    if(atBottom)box.scrollTop=box.scrollHeight;\n"
          "  }).catch(function(){});\n"
          "  dmPollT=setTimeout(fetchDmThread,3000);\n"
          "}\n"
          "function sendDm(e){\n"
          "  e.preventDefault();\n"
          "  if(!dmPub)return;\n"
          "  var txt=document.getElementById('dm-txt').value.trim();\n"
          "  if(!txt)return;\n"
          "  fetch('/api/dm/send',{method:'POST',credentials:'include',\n"
          "    headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
          "    body:'pub='+encodeURIComponent(dmPub)+'&text='+encodeURIComponent(txt)})\n"
          "  .then(function(r){\n"
          "    if(r.ok){document.getElementById('dm-txt').value='';fetchDmThread();}\n"
          "    else r.text().then(function(t){alert('DM fout: '+t);});\n"
          "  });\n"
          "}\n"
          "fetchMsgs();fetchNicks();\n"
          "</script>\n";

  page += FPSTR(HTML_FOOT);
  return page;
}

// ---------------------------------------------------------------------------
//  ACL management page (JES-720) — admin-only, no serial required
// ---------------------------------------------------------------------------
String WebManager::buildAclPage() {
  String page = buildHead(_mesh.getNodeName());

  page += "<div id='topbar'><h1>&#128100; ACL &mdash; ";
  page += htmlEscape(_mesh.getNodeName());
  page += "</h1><a href='/'>&#8592; Beheer</a></div>"
          "<div style='padding:10px'>"
          "<p style='color:#aaa;font-size:0.88em;margin:0 0 10px'>Beheer wie in elke room mag schrijven. "
          "Sla op om rechten direct te wijzigen (geen herstart nodig).</p>";

  // Role labels
  static const char* ROLE_NAMES[] = { "GUEST (geen toegang)", "Read-only", "Read-write", "ADMIN" };

  for (int r = 0; r < MAX_ROOMS; r++) {
    if (!_mesh.isRoomActive(r)) continue;
    int nc = _mesh.getRoomNumClients(r);

    page += "<div class='card'><h3>";
    page += htmlEscape(_mesh.getRoomName(r));
    page += " &mdash; ";
    page += nc;
    page += " client(s)</h3>";

    if (nc == 0) {
      page += "<p style='color:#aaa'>Geen ingelogde clients.</p>";
    } else {
      page += "<table style='width:100%;border-collapse:collapse'>"
              "<tr><th style='text-align:left;padding:4px 8px'>Naam</th>"
              "<th style='text-align:left;padding:4px 8px'>PubKey (8 hex)</th>"
              "<th style='text-align:left;padding:4px 8px'>Rol</th>"
              "<th style='padding:4px 8px'>Actie</th></tr>";

      for (int c = 0; c < nc; c++) {
        const ClientInfo* ci = _mesh.getRoomClient(r, c);
        if (!ci) continue;

        // Build 8-char hex prefix of pub_key
        char pub8[9] = {};
        for (int b = 0; b < 4; b++)
          snprintf(pub8 + b * 2, 3, "%02x", (unsigned int)ci->id.pub_key[b]);

        uint8_t perm = ci->permissions & 3;
        const char* name = _mesh.resolveName(ci->id.pub_key);

        page += "<tr style='border-top:1px solid #333'>";
        page += "<td style='padding:4px 8px'>";
        page += htmlEscape(name);
        page += "</td><td style='padding:4px 8px;font-family:monospace'>";
        page += pub8;
        page += "</td><td style='padding:4px 8px'>";
        page += ROLE_NAMES[perm];
        page += "</td><td style='padding:4px 8px'>"
                "<form method='POST' action='/api/acl/set' style='display:inline'>"
                "<input type='hidden' name='room' value='";
        page += r;
        page += "'><input type='hidden' name='pub' value='";
        page += pub8;
        page += "'><select name='perm'>";
        for (int p = 0; p <= 3; p++) {
          page += "<option value='";
          page += p;
          page += "'";
          if (p == perm) page += " selected";
          page += ">";
          page += ROLE_NAMES[p];
          page += "</option>";
        }
        page += "</select> <button type='submit'>Opslaan</button></form></td></tr>";
      }
      page += "</table>";
    }
    page += "</div>";
  }

  page += "</div>";  // padding wrapper
  page += FPSTR(HTML_FOOT);
  return page;
}

/* ---- /api/stats JSON (JES-800) ----------------------------------------- */
String WebManager::buildStatsJson() {
  MultiRoomMesh& mesh = _mesh;
  String j = "{";
  // Totals
  j += "\"uptime_s\":";      j += (unsigned long)(mesh.getUptimeMillis() / 1000UL);
  j += ",\"total_posts\":";  j += (unsigned long)mesh.getTotalPosts();
  j += ",\"total_contacts\":"; j += (int)mesh.getTotalContacts();
  j += ",\"num_rooms\":";    j += mesh.getNumActiveRooms();

  // Histogram (newest bucket = index 0)
  j += ",\"hist\":[";
  for (int b = 0; b < HIST_BUCKETS; b++) {
    if (b) j += ",";
    j += (unsigned)mesh.getHistBucket(b);
  }
  j += "]";

  // Per-room array
  j += ",\"rooms\":[";
  bool first_room = true;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!mesh.isRoomActive(i)) continue;
    if (!first_room) j += ",";
    first_room = false;
    int nc = mesh.getRoomNumClients(i);
    int adm = 0, rw = 0, ro = 0, guest = 0;
    for (int k = 0; k < nc; k++) {
      auto c = mesh.getRoomClient(i, k);
      if (!c) continue;
      switch (c->permissions & PERM_ACL_ROLE_MASK) {
        case PERM_ACL_ADMIN:      adm++;   break;
        case PERM_ACL_READ_WRITE: rw++;    break;
        case PERM_ACL_READ_ONLY:  ro++;    break;
        default:                  guest++; break;
      }
    }
    j += "{\"idx\":"; j += i;
    j += ",\"name\":\""; j += jsonEscape(mesh.getRoomName(i)); j += "\"";
    j += ",\"posts\":"; j += mesh.getRoomPostCount(i);
    j += ",\"clients\":"; j += nc;
    j += ",\"admin\":"; j += adm;
    j += ",\"rw\":"; j += rw;
    j += ",\"ro\":"; j += ro;
    j += ",\"guest\":"; j += guest;

    // Per-client array (public info only — no private keys/passwords)
    j += ",\"users\":[";
    bool first_u = true;
    for (int k = 0; k < nc; k++) {
      auto c = mesh.getRoomClient(i, k);
      if (!c) continue;
      if (!first_u) j += ",";
      first_u = false;
      char hex[9];
      mesh::Utils::toHex(hex, c->id.pub_key, 4); hex[8] = 0;
      const char* nm = mesh.resolveName(c->id.pub_key);
      uint8_t role = c->permissions & PERM_ACL_ROLE_MASK;
      j += "{\"pub\":\""; j += hex; j += "\"";
      j += ",\"name\":\""; j += jsonEscape(nm); j += "\"";
      j += ",\"role\":"; j += (int)role;
      j += ",\"hops\":";
      if (c->out_path_len == OUT_PATH_UNKNOWN) j += "-1";
      else j += (int)c->out_path_len;
      j += ",\"rssi\":"; j += (int)c->last_rssi;
      j += ",\"snr\":";  j += (int)c->last_snr;
      j += ",\"msgs\":"; j += (unsigned)c->msg_count;
      j += "}";
    }
    j += "]}";
  }
  j += "]}";
  return j;
}

/* ---- /stats HTML page (JES-800) ----------------------------------------- */
String WebManager::buildStatsPage() {
  MultiRoomMesh& mesh = _mesh;
  String page = buildHead(mesh.getNodeName());

  page += "<div id='topbar'><h1>&#128200; Statistieken &mdash; ";
  page += htmlEscape(mesh.getNodeName());
  page += "</h1><a href='/'>&#8592; Beheer</a></div>"
          "<div style='padding:10px'>";

  // Totals summary
  unsigned long upSec = (unsigned long)(mesh.getUptimeMillis() / 1000UL);
  unsigned long d = upSec / 86400, h = (upSec % 86400) / 3600;
  unsigned long m = (upSec % 3600) / 60, s = upSec % 60;
  page += "<table><tr><th>Uptime</th><td>";
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%lud %02lu:%02lu:%02lu", d, h, m, s);
  page += uptimeBuf;
  page += "</td></tr><tr><th>Actieve rooms</th><td>"; page += mesh.getNumActiveRooms();
  page += "</td></tr><tr><th>Totaal berichten</th><td>"; page += (unsigned long)mesh.getTotalPosts();
  page += "</td></tr><tr><th>Totaal contacts</th><td>"; page += (int)mesh.getTotalContacts();
  page += "</td></tr></table></div>";

  // Per-room tables
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!mesh.isRoomActive(i)) continue;
    int nc = mesh.getRoomNumClients(i);
    page += "<div class='card'><h3>Room ";
    page += i; page += " &mdash; "; page += htmlEscape(mesh.getRoomName(i));
    page += " (";  page += nc; page += " clients, ";
    page += mesh.getRoomPostCount(i); page += " berichten)</h3>";
    if (nc == 0) {
      page += "<p style='color:#aaa'>Geen verbonden clients.</p>";
    } else {
      page += "<table><tr><th>Pubkey</th><th>Naam</th><th>Rol</th><th>Hops</th>"
              "<th>RSSI</th><th>SNR</th><th>Berichten</th></tr>";
      for (int k = 0; k < nc; k++) {
        auto c = mesh.getRoomClient(i, k);
        if (!c) continue;
        char hex[9];
        mesh::Utils::toHex(hex, c->id.pub_key, 4); hex[8] = 0;
        const char* nm = mesh.resolveName(c->id.pub_key);
        uint8_t role = c->permissions & PERM_ACL_ROLE_MASK;
        const char* role_s = (role == PERM_ACL_ADMIN) ? "admin"
                           : (role == PERM_ACL_READ_WRITE) ? "rw"
                           : (role == PERM_ACL_READ_ONLY)  ? "ro" : "guest";
        page += "<tr><td><code>"; page += hex; page += "</code></td>";
        page += "<td>"; page += htmlEscape(nm); page += "</td>";
        page += "<td>"; page += role_s; page += "</td>";
        page += "<td>";
        if (c->out_path_len == OUT_PATH_UNKNOWN) page += "?";
        else { page += (int)c->out_path_len; }
        page += "</td><td>"; page += (int)c->last_rssi;
        page += "</td><td>"; page += (int)c->last_snr;
        page += "</td><td>"; page += (unsigned)c->msg_count;
        page += "</td></tr>";
      }
      page += "</table>";
    }
    page += "</div>";
  }

  // 24-hour message histogram (CSS bar chart)
  page += "<div class='card'><h3>Berichtenhistogram (24u)</h3>";
  uint16_t maxVal = 1;
  for (int b = 0; b < HIST_BUCKETS; b++) {
    uint16_t v = mesh.getHistBucket(b);
    if (v > maxVal) maxVal = v;
  }
  page += "<div style='display:flex;align-items:flex-end;gap:2px;height:80px;"
          "background:#1a1a2e;padding:4px;border-radius:4px'>";
  for (int b = HIST_BUCKETS - 1; b >= 0; b--) {
    uint16_t v = mesh.getHistBucket(b);
    int pct = (int)((uint32_t)v * 100 / maxVal);
    page += "<div title='-"; page += b; page += "h: "; page += (unsigned)v;
    page += "' style='flex:1;background:#00d4ff;height:";
    page += pct; page += "%;min-height:2px'></div>";
  }
  page += "</div>";
  page += "<p style='font-size:0.8em;color:#aaa'>"
          "Elke balk = 1 uur. Links = 23 uur geleden, rechts = huidig uur.</p>";
  page += "</div>";

  page += "</div>";  // padding wrapper
  page += FPSTR(HTML_FOOT);
  return page;
}

/* ---- Debug log JSON (JES-852) ------------------------------------------- */
String WebManager::buildDebugLogJson() {
  String j = "{\"enabled\":";
  j += _mesh.isDebugLogEnabled() ? "true" : "false";
  j += ",\"count\":";
  j += (int)g_dbglog.count();
  j += ",\"entries\":[";
  uint16_t cnt = g_dbglog.count();
  // Return up to last 200 entries newest-last (oldest first in array)
  // Entries are already ordered oldest-first by DebugLog::get()
  for (uint16_t i = 0; i < cnt; i++) {
    const DebugEntry& e = g_dbglog.get(i);
    if (i > 0) j += ',';
    j += "{\"ts\":";
    j += (unsigned long)e.ts;
    j += ",\"msg\":\"";
    // JSON-escape the message (reuse existing jsonEscape helper if available,
    // otherwise do minimal escaping: backslash and double-quote)
    for (const char* p = e.msg; *p; p++) {
      char c = *p;
      if      (c == '"')  j += "\\\"";
      else if (c == '\\') j += "\\\\";
      else if (c == '\n') j += "\\n";
      else if (c == '\r') j += "\\r";
      else                j += c;
    }
    j += "\"}";
  }
  j += "]}";
  return j;
}

// Stream the main status page directly to an AsyncResponseStream.
// Uses PrintSink so the existing page += ... code works without a large
// ---------------------------------------------------------------------------
//  Page navigation bar — links to all main pages (JES-854 split)
// ---------------------------------------------------------------------------
static void writePageNav(PrintSink& page, const char* active) {
  struct NavItem { const char* href; const char* label; const char* id; };
  static const NavItem items[] = {
    {"/",        "Status",    "status"},
    {"/rooms",   "Rooms",     "rooms"},
    {"/network", "Netwerk",   "network"},
    {"/system",  "Systeem",   "system"},
    {"/chat",    "Berichten", "chat"},
    {"/acl",     "ACL",       "acl"},
    {"/stats",   "Stats",     "stats"},
  };
  page += "<div id='tabnav'>";
  for (int i = 0; i < 7; i++) {
    bool act = strcmp(active, items[i].id) == 0;
    page += "<a href='"; page += items[i].href;
    page += act ? "' class='tnb act'>" : "' class='tnb'>";
    page += items[i].label;
    page += "</a>";
  }
  page += "</div>";
}

// Uses AsyncResponseStream (JES-854) — chunked, avoids large contiguous heap alloc.
void WebManager::buildStatusPageStream(AsyncResponseStream& out, const char* ip) {
  MultiRoomMesh& mesh = _mesh;
  WifiMode mode       = _mode;
  const char* ap_ssid = _ap_ssid;
  const char* sta_ssid = _sta_ssid;

  PrintSink page(out);
  page += buildHead(mesh.getNodeName());

  // ---- Top bar ----
  page += "<div id='topbar'><h1>";
  page += htmlEscape(mesh.getNodeName());
  page += " Room Server</h1></div>";

  // ---- Page nav bar ----
  writePageNav(page, "status");

  // ================================================================
  // STATUS PAGE CONTENT
  // ================================================================
  page += "<div style='padding:12px 10px'>";

  // Node summary card
  page += "<div class='card'>";
  page += "<table>";
  page += "<tr><th>Firmware</th><td>" FIRMWARE_VERSION "</td></tr>";
  page += "<tr><th>Build</th><td>" FIRMWARE_BUILD_DATE "</td></tr>";
  page += "<tr><th>WiFi</th><td>"; page += (mode == MODE_AP ? "AP (hotspot)" : "STA (client)");
  page += "</td></tr><tr><th>IP</th><td>"; page += ip;
  page += "</td></tr><tr><th>Rooms actief</th><td>"; page += mesh.getNumActiveRooms();
  page += " / "; page += MAX_ROOMS; page += "</td></tr>";
  {
    uint32_t now_ts = mesh.getRTCClock()->getCurrentTime();
    time_t t = (time_t)now_ts;
    char tbuf[32] = "niet gesynchroniseerd";
    if (now_ts > 1000000000UL) {
      struct tm ti{};
      gmtime_r(&t, &ti);
      strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M UTC", &ti);
    }
    page += "<tr><th>Klok</th><td>"; page += tbuf;
    if (_ntp_synced) page += " <span style='color:#00ff88'>(NTP &#10003;)</span>";
    page += "</td></tr>";
  }
  page += "</table>";
  // Server name edit form (JES-828)
  page += "<form method='post' action='/api/node/name' style='margin-top:12px'>"
          "<label style='display:block;color:#aaa;font-size:0.88em;margin-bottom:4px'>Server naam</label>"
          "<div style='display:flex;gap:6px'>"
          "<input name='name' value='";
  page += htmlEscape(mesh.getNodeName());
  page += "' maxlength='31' style='flex:1;min-width:0'>"
          "<button type='submit' style='flex:none'>Opslaan</button></div></form>";
  page += "</div>";

  // Quick navigation cards
  page += "<div class='card'>"
          "<div style='display:flex;flex-direction:column;gap:8px'>"
          "<a href='/rooms' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#127979; Rooms &mdash; rooms, zichtbaarheid, notificaties</button>"
          "</a>"
          "<a href='/network' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#128246; Netwerk &mdash; WiFi, LoRa, peers, sync</button>"
          "</a>"
          "<a href='/system' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#9881; Systeem &mdash; backup, OLED, MQTT, OTA, debug</button>"
          "</a>"
          "<a href='/chat' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#128172; Berichten &mdash; berichten bekijken en posten</button>"
          "</a>"
          "<a href='/acl' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#128100; ACL beheer &mdash; rechten per gebruiker</button>"
          "</a>"
          "<a href='/stats' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px'>&#128200; Statistieken &mdash; RSSI/SNR, histogram</button>"
          "</a>"
          "</div></div>";

  page += "</div>";  // end status content wrapper

  page += FPSTR(HTML_FOOT);
  // (void return — PrintSink wrote directly to the AsyncResponseStream)
}

// ---------------------------------------------------------------------------
//  /rooms — Room management page (JES-854 split)
// ---------------------------------------------------------------------------
void WebManager::buildRoomsPageStream(AsyncResponseStream& out) {
  MultiRoomMesh& mesh = _mesh;
  PrintSink page(out);
  page += buildHead(mesh.getNodeName());
  page += "<div id='topbar'><h1>";
  page += htmlEscape(mesh.getNodeName());
  page += " &mdash; Rooms</h1></div>";
  writePageNav(page, "rooms");
  page += "<div style='padding:12px 10px'>";
  // editRoom helper — pre-fills the edit form and scrolls to it
  page += "<script>"
          "function editRoom(i,n){"
            "document.getElementById('editIdx').value=i;"
            "document.getElementById('editName').value=n;"
            "document.getElementById('editClearPass').checked=false;"
            "document.getElementById('editClearGuest').checked=false;"
            "document.getElementById('editCard').scrollIntoView({behavior:'smooth'});"
          "}"
          "</script>";

  // Rooms overview table
  page += "<div class='card'><h2>Rooms</h2>";
  page += "<table><tr><th>#</th><th>Naam</th><th>Status</th><th>Clients</th><th>Posts</th><th>Acties</th></tr>";
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!mesh.isRoomActive(i)) continue;
    bool stealth_i = mesh.isRoomStealth(i);
    page += "<tr><td>"; page += i;
    page += "</td><td>"; page += htmlEscape(mesh.getRoomName(i));
    page += "</td><td>";
    page += stealth_i ? "<span class='warn'>STEALTH</span>" : "<span class='ok'>VISIBLE</span>";
    page += "</td><td>"; page += mesh.getRoomClientCount(i);
    page += "</td><td>"; page += mesh.getRoomPostCount(i);
    page += "</td><td><div class='btngrp'>";
    // Edit button — pre-fills the edit form below and scrolls to it
    {
      page += "<button type='button' data-idx='"; page += i;
      page += "' data-name=\""; page += htmlEscape(mesh.getRoomName(i));
      page += "\" onclick=\"editRoom(this.dataset.idx,this.dataset.name)\">Bewerken</button>";
    }
    // Stealth toggle
    page += "<form method='post' action='/api/room/stealth'>"
            "<input type='hidden' name='idx' value='"; page += i;
    page += "'><input type='hidden' name='stealth' value='";
    page += stealth_i ? "off" : "on";
    page += "'><button type='submit'>";
    page += stealth_i ? "Zichtbaar" : "Verberg";
    page += "</button></form>";
    // QR join
    page += "<a href='/api/room/qr?idx="; page += i;
    page += "' style='text-decoration:none'><button>QR</button></a>";
    // Rekey — dataset avoids JS-string-literal injection
    {
      const char* exp_name = (i == 0) ? "REKEY" : mesh.getRoomName(i);
      page += "<form method='post' action='/api/room/rekey'>"
              "<input type='hidden' name='idx' value='"; page += i;
      page += "'><input type='hidden' name='confirm' value=''>"
              "<button type='button' class='warn-btn' data-exp=\""; page += htmlEscape(exp_name);
      page += "\" onclick=\""
              "var e=this.dataset.exp;"
              "var w="; page += (i == 0) ? "'Room 0 = node-identiteit. Alle peer-links verbreken!'" : "'Bestaande QR-codes en join-URIs worden ongeldig.'";
      page += ";"
              "var c=window.prompt('Rekey room "; page += i;
      page += " (\\'' + e + '\\')?\\n' + w + '\\nTyp \\'' + e + '\\' ter bevestiging:');"
              "if(c!==null){"
              "this.closest('form').elements.namedItem('confirm').value=c;"
              "this.closest('form').submit();}"
              "\">Rekey</button></form>";
    }
    if (i > 0) {
      page += "<form method='post' action='/api/room/del'>"
              "<input type='hidden' name='idx' value='"; page += i;
      page += "'><button class='del' onclick=\"return confirm('Delete room "; page += i;
      page += "?')\">Del</button></form>";
    }
    page += "</div></td></tr>";
  }
  page += "</table>";
  page += "<form method='post' action='/api/room/add' style='margin-top:8px'>"
          "<button type='submit'>+ Room toevoegen</button></form></div>";

  // Edit room form
  page += "<div class='card' id='editCard'><h2>Room bewerken</h2>"
          "<form method='post' action='/api/room/set'>"
          "<div class='frow'><label>Idx</label><input id='editIdx' name='idx' type='number' min='0' max='15'></div>"
          "<div class='frow'><label>Naam</label><input id='editName' name='name' maxlength='23'></div>"
          "<div class='frow'><label>Wachtwoord</label>"
          "<input name='pass' maxlength='15' placeholder='leeg laten = ongewijzigd'> "
          "<label style='font-size:0.85em;font-weight:normal'>"
          "<input type='checkbox' id='editClearPass' name='clear_pass' value='1'> wissen</label></div>"
          "<div class='frow'><label>Gast-ww</label>"
          "<input name='guest' maxlength='15' placeholder='leeg laten = ongewijzigd'> "
          "<label style='font-size:0.85em;font-weight:normal'>"
          "<input type='checkbox' id='editClearGuest' name='clear_guest' value='1'> wissen</label></div>"
          "<button type='submit'>Opslaan</button></form></div>";

  // Visibility (stealth) — global toggle
  {
    bool all_stealth = true;
    bool any_active  = false;
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!mesh.isRoomActive(i)) continue;
      any_active = true;
      if (!mesh.isRoomStealth(i)) { all_stealth = false; break; }
    }
    page += "<div class='card'><h2>Zichtbaarheid</h2>";
    if (any_active && all_stealth) {
      page += "<p>Alle rooms: <b class='warn'>STEALTH</b> &mdash; geen adverts verstuurd. "
              "Rooms zijn joinbaar via QR/uri als je de sleutel kent.</p>";
    } else {
      page += "<p>Een of meer rooms: <b class='ok'>ZICHTBAAR</b> &mdash; adverts actief.</p>";
    }
    page += "<div style='display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px'>"
            "<form method='post' action='/api/stealth'>"
            "<button name='stealth' value='off'>Alles zichtbaar</button></form>"
            "<form method='post' action='/api/stealth'>"
            "<button class='warn-btn' name='stealth' value='on' "
            "onclick=\"return confirm('Alle rooms verbergen? Adverts stoppen.')\">Alles verbergen</button></form>"
            "</div>";
    char adv_sec_str[8];
    snprintf(adv_sec_str, sizeof(adv_sec_str), "%d", (int)mesh.getAdvertIntervalSec());
    page += "<form method='post' action='/api/advert/interval'>"
            "<div class='frow'><label>Advert interval</label>"
            "<input name='seconds' type='number' min='10' max='3600' value='";
    page += adv_sec_str;
    page += "'> s</div>"
            "<button type='submit'>Opslaan</button></form>"
            "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>"
            "10&ndash;3600 s, standaard 120 s. CLI: <code>advert interval &lt;s&gt;</code></p>"
            "</div>";
  }

  // Repeater modus (JES-855) — independent of stealth
  {
    const NodePrefs* p = mesh.getNodePrefs();
    bool rep_on = (p->disable_fwd == 0);
    page += "<div class='card'><h2>Repeater modus</h2>";
    if (rep_on) {
      page += "<p>Status: <b class='ok'>AAN</b> &mdash; node stuurt LoRa-berichten van andere nodes door.</p>";
    } else {
      page += "<p>Status: <b class='warn'>UIT</b> &mdash; node stuurt alleen eigen berichten.</p>";
    }
    page += "<p style='font-size:0.84em;color:#aaa;margin:4px 0 10px'>"
            "Onafhankelijk van stealth: stealth regelt zichtbaarheid van rooms, "
            "repeater regelt doorsturen van mesh-pakketten.</p>"
            "<div style='display:flex;gap:8px;flex-wrap:wrap'>"
            "<form method='post' action='/api/repeater'>"
            "<button name='repeater' value='on'";
    if (rep_on) page += " class='ok-btn'";
    page += ">Repeater AAN</button></form>"
            "<form method='post' action='/api/repeater'>"
            "<button name='repeater' value='off'";
    if (!rep_on) page += " class='warn-btn'";
    page += ">Repeater UIT</button></form>"
            "</div>"
            "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>"
            "CLI: <code>repeater on|off|status</code></p></div>";
  }

  // Login notification targets (JES-834)
  {
    page += "<div class='card'><h2>Login Notificaties</h2>"
            "<p style='font-size:0.85em;color:#aaa'>"
            "Stuur een DM naar deze pubkeys bij elke inlogpoging. Max ";
    page += MAX_NOTIFY_TARGETS;
    page += " per room.</p>";
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!mesh.isRoomActive(i)) continue;
      int cnt = mesh.getNotifyTargetCount(i);
      page += "<p><b>Room "; page += i; page += " &mdash; ";
      page += htmlEscape(mesh.getRoomName(i));
      page += "</b> ("; page += cnt; page += "/"; page += MAX_NOTIFY_TARGETS; page += ")";
      if (cnt > 0) {
        page += "<br>";
        for (int t = 0; t < cnt; t++) {
          const uint8_t* k = mesh.getNotifyTarget(i, t);
          char hex[65] = {};
          for (int b = 0; b < PUB_KEY_SIZE; b++) snprintf(hex + b * 2, 3, "%02x", k[b]);
          page += "<code>"; page += String(hex).substring(0, 8); page += "&hellip;</code> ";
          page += "<form method='post' action='/api/room/notify/del' style='display:inline'>"
                  "<input type='hidden' name='idx' value='"; page += i; page += "'>"
                  "<input type='hidden' name='pubkey' value='"; page += hex; page += "'>"
                  "<button type='submit' style='min-height:32px;padding:2px 10px'>&#x2715;</button>"
                  "</form> ";
        }
      }
      if (cnt < MAX_NOTIFY_TARGETS) {
        page += "<form method='post' action='/api/room/notify/add' style='margin-top:6px'>"
                "<input type='hidden' name='idx' value='"; page += i; page += "'>"
                "<input name='pubkey' maxlength='64' placeholder='64-hex pubkey van companion'>"
                "<button type='submit' style='margin-top:6px'>Toevoegen</button>"
                "</form>";
      }
      page += "</p>";
    }
    page += "</div>";
  }

  page += "</div>";  // end content wrapper
  page += FPSTR(HTML_FOOT);
}

// ---------------------------------------------------------------------------
//  /network — Network settings page (JES-854 split)
// ---------------------------------------------------------------------------
void WebManager::buildNetworkPageStream(AsyncResponseStream& out, const char* ip) {
  MultiRoomMesh& mesh = _mesh;
  WifiMode mode        = _mode;
  const char* ap_ssid  = _ap_ssid;
  const char* sta_ssid = _sta_ssid;
  PrintSink page(out);
  page += buildHead(mesh.getNodeName());
  page += "<div id='topbar'><h1>";
  page += htmlEscape(mesh.getNodeName());
  page += " &mdash; Netwerk</h1></div>";
  writePageNav(page, "network");
  page += "<div style='padding:12px 10px'>";

  // WiFi config
  page += "<div class='card'><h2>WiFi</h2>";
  page += "<form method='post' action='/api/wifi/mode'>"
          "<div class='frow'><label>Mode</label><select name='mode'>"
          "<option value='ap'"; page += (mode == MODE_AP ? " selected" : ""); page += ">AP (eigen hotspot)</option>"
          "<option value='sta'"; page += (mode == MODE_STA ? " selected" : ""); page += ">STA (verbinden met netwerk)</option>"
          "</select></div>"
          "<button type='submit'>Wissel modus</button></form>";

  page += "<hr style='border-color:#2a3050;margin:12px 0'>"
          "<h3>AP instellingen</h3>"
          "<form method='post' action='/api/wifi/ap'>"
          "<div class='frow'><label>SSID</label><input name='ssid' value='"; page += ap_ssid;
  page += "' maxlength='32'></div>"
          "<div class='frow'><label>Wachtwoord</label><input name='pass' type='password' maxlength='63' placeholder='leeg = open'></div>"
          "<button type='submit'>AP opslaan</button></form>";

  page += "<hr style='border-color:#2a3050;margin:12px 0'>"
          "<h3>STA instellingen</h3>"
          "<form method='post' action='/api/wifi/sta'>"
          "<div class='frow'><label>SSID</label><input name='ssid' value='"; page += sta_ssid;
  page += "' maxlength='32'></div>"
          "<div class='frow'><label>Wachtwoord</label><input name='pass' type='password' maxlength='63'></div>"
          "<button type='submit'>STA opslaan &amp; verbinden</button></form>"
          "<p style='margin-top:8px'>Huidig IP: <b>"; page += ip; page += "</b></p>"
          "<hr style='border-color:#2a3050;margin:12px 0'>"
          "<h3>Klok</h3>"
          "<p style='font-size:0.9em;color:#aaa'>NTP synchroniseert automatisch zodra de node verbinding heeft met het internet (STA-modus). "
          "Als NTP niet beschikbaar is, gebruik dan de knop hieronder om de klok in te stellen op de tijd van je browser.</p>"
          "<button type='button' class='sec' onclick='syncBrowserTime()'>&#128337; Synchroniseer browsertijd</button>"
          "<span id='ts-status' style='margin-left:10px;font-size:0.85em;color:#aaa'></span>"
          "<script>"
          "function syncBrowserTime(){"
          "  var ts=Math.floor(Date.now()/1000);"
          "  fetch('/api/set_time',{method:'POST',credentials:'include',"
          "    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "    body:'ts='+ts})"
          "  .then(function(r){"
          "    document.getElementById('ts-status').textContent=r.ok?'Klok ingesteld!':'Fout bij instellen';"
          "    document.getElementById('ts-status').style.color=r.ok?'#00ff88':'#ff4444';"
          "  }).catch(function(){document.getElementById('ts-status').textContent='Verbindingsfout';});"
          "}"
          "</script>"
          "<hr style='border-color:#2a3050;margin:12px 0'>"
          "<h3>NTP server</h3>"
          "<p style='font-size:0.9em;color:#aaa'>Huidige status: ";
  page += _ntp_synced ? "<span style='color:#00ff88'>&#10003; Gesynchroniseerd</span>"
                      : "<span style='color:#aaa'>Niet gesynchroniseerd (of AP-modus)</span>";
  page += "</p>"
          "<form method='post' action='/api/ntp'>"
          "<div class='frow'><label>NTP server</label>"
          "<input name='server' value='";
  page += htmlEscape(_ntp_server);
  page += "' maxlength='63' placeholder='pool.ntp.org'>"
          "</div>"
          "<button type='submit'>Opslaan &amp; herverbinden</button>"
          "</form>"
          "</div>";

  // LoRa radio settings
  {
    const NodePrefs* p = mesh.getNodePrefs();
    char freq_str[16], bw_str[16], sf_str[8], cr_str[8], tx_str[8];
    snprintf(freq_str, sizeof(freq_str), "%.3f", (double)p->freq);
    snprintf(bw_str,   sizeof(bw_str),   "%.2f", (double)p->bw);
    snprintf(sf_str,   sizeof(sf_str),   "%d",   (int)p->sf);
    snprintf(cr_str,   sizeof(cr_str),   "%d",   (int)p->cr);
    snprintf(tx_str,   sizeof(tx_str),   "%d",   (int)p->tx_power_dbm);

    page += "<div class='card'><h2>LoRa Radio</h2>";
    page += "<table>"
            "<tr><th>Freq (MHz)</th><td>"; page += freq_str;
    page += "</td></tr><tr><th>BW (kHz)</th><td>"; page += bw_str;
    page += "</td></tr><tr><th>SF</th><td>SF"; page += sf_str;
    page += "</td></tr><tr><th>CR</th><td>4/"; page += cr_str;
    page += "</td></tr><tr><th>TX Power</th><td>"; page += tx_str;
    page += " dBm</td></tr></table>";

    page += "<form method='post' action='/api/lora' style='margin-top:10px'>";
    page += "<div class='frow'><label>Freq (MHz)</label><input name='freq' value='"; page += freq_str; page += "'></div>";
    page += "<div class='frow'><label>BW (kHz)</label><select name='bw'>";
    static const float BW_OPTS[]   = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
    static const char* BW_LABELS[] = {"7.80","10.40","15.60","20.80","31.25","41.70","62.50","125.00","250.00","500.00"};
    for (int i = 0; i < 10; i++) {
      page += "<option value='"; page += BW_LABELS[i];
      if (fabsf(BW_OPTS[i] - p->bw) < 0.5f) page += "' selected='selected";
      page += "'>"; page += BW_LABELS[i]; page += "</option>";
    }
    page += "</select></div>";
    page += "<div class='frow'><label>SF</label><select name='sf'>";
    for (int sf = 5; sf <= 12; sf++) {
      page += "<option value='"; page += sf;
      if (sf == (int)p->sf) page += "' selected='selected";
      page += "'>SF"; page += sf; page += "</option>";
    }
    page += "</select></div>";
    page += "<div class='frow'><label>CR</label><select name='cr'>";
    for (int cr = 5; cr <= 8; cr++) {
      page += "<option value='"; page += cr;
      if (cr == (int)p->cr) page += "' selected='selected";
      page += "'>4/"; page += cr; page += "</option>";
    }
    page += "</select></div>";
    page += "<div class='frow'><label>TX (dBm)</label><input name='txpower' value='"; page += tx_str;
    page += "' type='number' min='2' max='22'></div>";
    page += "<button type='submit'>LoRa opslaan</button></form>"
            "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>"
            "Freq/BW/SF/CR vereisen herstart. TX power is live.</p></div>";
  }

  // MeshCore scope/regio instellingen (JES-852)
  {
    const NodePrefs* p = mesh.getNodePrefs();
    char fm_str[8], fmu_str[8], fma_str[8], phm_str[8];
    snprintf(fm_str,  sizeof(fm_str),  "%d", (int)p->flood_max);
    snprintf(fmu_str, sizeof(fmu_str), "%d", (int)p->flood_max_unscoped);
    snprintf(fma_str, sizeof(fma_str), "%d", (int)p->flood_max_advert);
    snprintf(phm_str, sizeof(phm_str), "%d", (int)p->path_hash_mode);

    page += "<div class='card'><h2>MeshCore Scope &amp; Regio</h2>";

    // Flood forwarding + flood limits
    page += "<form method='post' action='/api/meshcore' style='margin-bottom:8px'>"
            "<div class='frow'><label>Flood door sturen</label><select name='fwd'>"
            "<option value='on'"; page += (p->disable_fwd == 0) ? " selected" : ""; page += ">Aan (aanbevolen)</option>"
            "<option value='off'"; page += (p->disable_fwd != 0) ? " selected" : ""; page += ">Uit</option>"
            "</select></div>";
    page += "<div class='frow'><label>flood_max (scoped)</label>"
            "<input name='flood_max' type='number' min='1' max='255' value='"; page += fm_str;
    page += "'></div>"
            "<div class='frow'><label>flood_max_unscoped</label>"
            "<input name='flood_max_unscoped' type='number' min='1' max='255' value='"; page += fmu_str;
    page += "'></div>"
            "<div class='frow'><label>flood_max_advert</label>"
            "<input name='flood_max_advert' type='number' min='1' max='255' value='"; page += fma_str;
    page += "'></div>"
            "<div class='frow'><label>Path hash mode</label><select name='path_hash'>"
            "<option value='0'"; page += ((int)p->path_hash_mode == 0) ? " selected" : ""; page += ">0 &ndash; geen</option>"
            "<option value='1'"; page += ((int)p->path_hash_mode == 1) ? " selected" : ""; page += ">1 &ndash; normaal</option>"
            "<option value='2'"; page += ((int)p->path_hash_mode == 2) ? " selected" : ""; page += ">2 &ndash; streng</option>"
            "</select></div>"
            "<button type='submit'>Opslaan</button></form>";

    // Default scope (region)
    {
      const RegionEntry* def = mesh.getDefaultRegion();
      int rcount = mesh.getRegionCount();
      page += "<hr style='border-color:#2a3050;margin:10px 0'>"
              "<h3 style='margin:0 0 6px'>Standaard scope (regio)</h3>"
              "<p style='font-size:0.84em;color:#aaa;margin:0 0 8px'>"
              "Huidig: <b>"; page += def ? htmlEscape(def->name) : "geen (globaal)";
      page += "</b></p>";
      if (rcount > 0) {
        page += "<form method='post' action='/api/meshcore/region'>"
                "<div class='frow'><label>Regio</label><select name='region'>"
                "<option value='null'"; page += (!def) ? " selected" : ""; page += ">geen (globaal)</option>";
        for (int ri = 0; ri < rcount; ri++) {
          const RegionEntry* r = mesh.getRegionByIdx(ri);
          if (!r || r->isWildcard()) continue;
          String esc = htmlEscape(r->name);
          page += "<option value='"; page += esc;
          if (def && strcmp(r->name, def->name) == 0) page += "' selected='selected";
          page += "'>"; page += esc; page += "</option>";
        }
        page += "</select></div>"
                "<button type='submit'>Scope instellen</button></form>";
      } else {
        page += "<p style='font-size:0.84em;color:#aaa'>Geen regio&apos;s geladen. "
                "Gebruik CLI: <code>region def &lt;naam&gt;</code></p>";
      }
    }

    page += "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>"
            "CLI: <code>set flood_max &lt;n&gt;</code> / <code>set fwd on|off</code> / "
            "<code>region default &lt;naam&gt;</code></p></div>";
  }

  // Peer-koppeling (JES-816)
  {
    const uint8_t* own_pub = mesh.getRoomPubKey(0);
    char own_hex[65] = {};
    if (own_pub) {
      for (int b = 0; b < PUB_KEY_SIZE; b++)
        snprintf(own_hex + b * 2, 3, "%02x", (unsigned int)own_pub[b]);
    }
    page += "<div class='card'><h2>Peer-koppeling</h2>";
    page += "<p style='font-size:0.88em;color:#aaa'>Eigen node pubkey &mdash; geef dit aan de operator van de andere node:</p>"
            "<p class='hex'>"; page += own_hex; page += "</p>";

    int np = mesh.getNumPeers();
    page += "<p style='margin:6px 0'>"; page += np; page += " / "; page += MAX_PEERS; page += " peers</p>";
    if (np > 0) {
      page += "<table><tr><th>#</th><th>Naam</th><th>Pubkey</th><th>Contact</th><th>Acties</th></tr>";
      for (int i = 0; i < MAX_PEERS; i++) {
        const PeerInfo* peer = mesh.getPeer(i);
        if (!peer || !peer->active) continue;
        char pfx[9] = {};
        for (int b = 0; b < 4; b++)
          snprintf(pfx + b * 2, 3, "%02x", (unsigned int)peer->pub_key[b]);
        page += "<tr><td>"; page += i;
        page += "</td><td>"; page += htmlEscape(peer->name);
        page += "</td><td><code>"; page += pfx; page += "&hellip;</code>";
        page += "</td><td>";
        page += (peer->last_contact > 0 ? String((unsigned long)peer->last_contact) : "nooit");
        page += "</td><td><div class='btngrp'>";
        page += "<form method='post' action='/api/peer/sync'>"
                "<input type='hidden' name='idx' value='"; page += i;
        page += "'><button type='submit'>Sync</button></form>";
        page += "<form method='post' action='/api/peer/roomsync'>"
                "<input type='hidden' name='idx' value='"; page += i;
        page += "'><button type='submit'>Rooms</button></form>";
        page += "<form method='post' action='/api/peer/del'>"
                "<input type='hidden' name='idx' value='"; page += i;
        page += "'><button class='del' onclick=\"return confirm('Peer "; page += i;
        page += " verwijderen?')\">Del</button></form>";
        page += "</div></td></tr>";
      }
      page += "</table>";
    }
    page += "<h3 style='margin-top:12px'>Peer toevoegen</h3>"
            "<form method='post' action='/api/peer/add'>"
            "<div class='frow'><label>Pubkey</label><input name='pub' maxlength='64' placeholder='64 hex chars' required></div>"
            "<div class='frow'><label>Naam</label><input name='name' maxlength='23'></div>"
            "<button type='submit'>Toevoegen</button></form>";
    page += "<form method='post' action='/api/peer/sync' style='margin-top:8px'>"
            "<button type='submit'>Sync All Nu</button></form>"
            "<form method='post' action='/api/peer/roomsync' style='margin-top:4px'>"
            "<button type='submit'>Rooms Sync (stuur rooms naar peers)</button></form>"
            "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>CLI: "
            "<code>peer add &lt;hex64&gt; &lt;naam&gt;</code></p></div>";

    // Sync interval (JES-844)
    {
      char sync_sec_str[8];
      snprintf(sync_sec_str, sizeof(sync_sec_str), "%lu", (unsigned long)mesh.getSyncIntervalSec());
      page += "<div class='card'><h2>Sync Interval</h2>"
              "<form method='post' action='/api/sync/interval'>"
              "<div class='frow'><label>Anti-entropy interval</label>"
              "<input name='seconds' type='number' min='10' max='3600' value='";
      page += sync_sec_str;
      page += "'> s</div>"
              "<button type='submit'>Opslaan</button></form>"
              "<p style='font-size:0.82em;color:#aaa;margin-top:8px'>"
              "10&ndash;3600 s, standaard 180 s. CLI: <code>sync interval &lt;s&gt;</code><br>"
              "Nieuwe berichten worden instant gepusht; dit interval is de periodieke controle.</p>"
              "</div>";
    }

    // Sync diagnostics panel (JES-833)
    page += "<div class='card'><h2>Sync Diagnostiek</h2>"
            "<div id='sync-panel'><p style='color:#aaa'>Laden...</p></div>"
            "<p style='font-size:0.8em;color:#aaa'>Auto-refresh 10s &bull; RAM-counters (reset bij reboot)</p></div>"
            "<script>"
            "function renderSync(d){"
              "var el=document.getElementById('sync-panel');"
              "if(!el)return;"
              "while(el.firstChild)el.removeChild(el.firstChild);"
              "var cp=document.createElement('p');"
              "cp.style.fontSize='0.85em';"
              "cp.textContent='REQ: '+d.counters.sync_req_sent"
                "+'  DAT: '+d.counters.sync_dat_recv"
                "+'  Recv: '+d.counters.sync_posts_recv"
                "+'  Sent: '+d.counters.sync_posts_sent;"
              "el.appendChild(cp);"
              "if(d.rooms&&d.rooms.length){"
                "var rp=document.createElement('p');"
                "rp.style.fontSize='0.82em';"
                "var rb=document.createElement('b');rb.textContent='Room hashes: ';rp.appendChild(rb);"
                "d.rooms.forEach(function(r){"
                  "var s=document.createElement('span');"
                  "s.textContent='['+r.idx+'] '+r.name+' ('+r.hash+'...)  ';"
                  "rp.appendChild(s);"
                "});"
                "el.appendChild(rp);"
              "}"
              "if(!d.peers||!d.peers.length){"
                "var np=document.createElement('p');np.textContent='Geen peers.';el.appendChild(np);return;"
              "}"
              "var tbl=document.createElement('table');"
              "var hr=document.createElement('tr');"
              "['#','Naam','Status','Recv','Sent'].forEach(function(h){"
                "var th=document.createElement('th');th.textContent=h;hr.appendChild(th);"
              "});"
              "tbl.appendChild(hr);"
              "d.peers.forEach(function(p){"
                "var tr=document.createElement('tr');"
                "[p.idx,p.name,p.status,p.sync_posts_recv,p.sync_posts_sent"
                "].forEach(function(v){"
                  "var td=document.createElement('td');td.textContent=v;tr.appendChild(td);"
                "});"
                "tbl.appendChild(tr);"
              "});"
              "el.appendChild(tbl);"
            "}"
            "function loadSync(){"
              "fetch('/api/sync/status')"
                ".then(function(r){return r.json();})"
                ".then(renderSync)"
                ".catch(function(){"
                  "var el=document.getElementById('sync-panel');"
                  "if(el){var e=document.createElement('p');e.textContent='Fout.';el.appendChild(e);}"
                "});"
            "}"
            "loadSync();setInterval(loadSync,10000);"
            "</script>";
  }

  page += "</div>";  // end content wrapper
  page += FPSTR(HTML_FOOT);
}

// ---------------------------------------------------------------------------
//  /system — System settings page (JES-854 split)
// ---------------------------------------------------------------------------
void WebManager::buildSystemPageStream(AsyncResponseStream& out) {
  MultiRoomMesh& mesh = _mesh;
  PrintSink page(out);
  page += buildHead(mesh.getNodeName());
  page += "<div id='topbar'><h1>";
  page += htmlEscape(mesh.getNodeName());
  page += " &mdash; Systeem</h1></div>";
  writePageNav(page, "system");
  page += "<div style='padding:12px 10px'>";
  // Backup / Restore
  page += "<div class='card'><h2>Backup &amp; Restore</h2>";
  page += "<a href='/api/backup' style='text-decoration:none'>"
          "<button style='width:100%;text-align:left;padding:12px 16px;margin-bottom:8px'>"
          "&#8681; Download backup</button></a>"
          "<p style='font-size:0.82em;color:#aaa;margin-bottom:10px'>"
          "Exporteert alle instellingen + private keys als JSON.</p>";
  page += "<form method='post' action='/api/restore' enctype='multipart/form-data'>"
          "<label style='display:block;color:#aaa;font-size:0.88em;margin-bottom:6px'>Backup bestand</label>"
          "<input type='file' name='backup' accept='.json' style='margin-bottom:8px'>"
          "<button type='submit' class='del' style='width:100%' "
          "onclick=\"return confirm('Restore overschrijft alle instellingen en herstart. Doorgaan?')\">&#8679; Restore &amp; Herstart</button>"
          "</form></div>";

  // Varia — screensaver / OLED settings
  if (_ui_task) {
    bool    ss_on    = _ui_task->isSsEnabled();
    bool    keep_on  = _ui_task->isSsKeepOn();
    uint8_t page_sec = _ui_task->getSsPageSec();
    page += "<div class='card'><h2>Scherm (OLED)</h2>";
    page += "<p style='font-size:0.85em;color:#aaa'>Screensaver beschermt het OLED-scherm tegen inbranding.</p>";
    page += "<form method='post' action='/api/screensaver'>";
    page += "<div class='frow'><label>Screensaver</label><select name='ss'>"
            "<option value='on'"; page += ss_on ? " selected" : ""; page += ">Aan (aanbevolen)</option>"
            "<option value='off'"; page += !ss_on ? " selected" : ""; page += ">Uit</option>"
            "</select></div>";
    page += "<div class='frow'><label>Scherm aan</label><select name='keep'>"
            "<option value='off'"; page += !keep_on ? " selected" : ""; page += ">Uit na 60s</option>"
            "<option value='on'"; page += keep_on ? " selected" : ""; page += ">Altijd aan</option>"
            "</select></div>";
    page += "<div class='frow'><label>Wisseltijd</label><select name='interval'>";
    const uint8_t opts[] = {1, 2, 3, 5, 10, 15, 20, 30, 60};
    for (int i = 0; i < (int)(sizeof(opts)/sizeof(opts[0])); i++) {
      page += String("<option value='") + opts[i] + "'";
      if (opts[i] == page_sec) page += " selected";
      page += String(">") + opts[i] + "s</option>";
    }
    page += "</select></div>";
    page += "<button type='submit'>Opslaan</button></form></div>";
  }

  // MQTT section (JES-792)
  if (_mqtt_mgr) {
    page += _mqtt_mgr->buildWebSection();
  }

  // GitHub self-update (JES-774)
  page += _ota_mgr.buildWebSection();

  // Legacy ElegantOTA serial-upload link (fallback)
  page += "<div class='card'><p style='font-size:0.85em'>"
          "<a href='/update'>Handmatige OTA upload (ElegantOTA)</a></p></div>";

  // Debug Log (JES-852) — RAM-only ring buffer, admin-gated
  {
    bool dlog_on = mesh.isDebugLogEnabled();
    page += "<div class='card'><h2>Debug Log</h2>"
            "<p style='font-size:0.84em;color:#aaa'>"
            "RAM-ring ("; page += (int)(DEBUG_LOG_MAX_ENTRIES);
    page += " regels max, ~"; page += (int)(DEBUG_LOG_MAX_ENTRIES * (4 + DEBUG_LOG_MSG_LEN) / 1024);
    page += " KB). Niet persistent: leeg na herstart.</p>"
            "<form method='post' action='/api/debuglog/enable' style='display:inline;margin-right:8px'>"
            "<input type='hidden' name='on' value='"; page += dlog_on ? "0" : "1";
    page += "'><button type='submit'>"; page += dlog_on ? "Logging UIT" : "Logging AAN";
    page += "</button></form>"
            "<form method='post' action='/api/debuglog/clear' style='display:inline'>"
            "<button type='submit' class='del'>Clear</button></form>"
            "<div id='dlog-panel' style='margin-top:10px'>"
            "<p style='color:#aaa;font-size:0.85em'>"; page += dlog_on ? "Laden..." : "Logging uitgeschakeld.";
    page += "</p></div>";
    if (dlog_on) {
      page += "<script>"
              "function loadDlog(){"
                "fetch('/api/debuglog',{credentials:'include'})"
                  ".then(function(r){return r.json();})"
                  ".then(function(d){"
                    "var el=document.getElementById('dlog-panel');"
                    "if(!el)return;"
                    "while(el.firstChild)el.removeChild(el.firstChild);"
                    "if(!d.entries||!d.entries.length){"
                      "var p=document.createElement('p');p.style.color='#aaa';"
                      "p.textContent='Geen log-regels.';el.appendChild(p);return;"
                    "}"
                    "var pre=document.createElement('pre');"
                    "pre.style.cssText='background:#111;padding:8px;border-radius:4px;"
                                       "font-size:0.78em;overflow-x:auto;max-height:400px;overflow-y:auto';"
                    "d.entries.forEach(function(e){"
                      "var ln=document.createTextNode('['+e.ts+'] '+e.msg+'\\n');"
                      "pre.appendChild(ln);"
                    "});"
                    "el.appendChild(pre);"
                    "pre.scrollTop=pre.scrollHeight;"
                  "})"
                  ".catch(function(){"
                    "var el=document.getElementById('dlog-panel');"
                    "if(el){var p=document.createElement('p');p.textContent='Fout.';el.appendChild(p);}"
                  "});"
              "}"
              "loadDlog();setInterval(loadDlog,3000);"
              "</script>";
    }
    page += "</div>";
  }

  page += "</div>";  // end content wrapper
  page += FPSTR(HTML_FOOT);
}

// ---------------------------------------------------------------------------
//  QR code page — per-room join QR rendered via inline canvas JS
// ---------------------------------------------------------------------------
static String buildQrPage(MultiRoomMesh& mesh, int idx) {
  String page = buildHead(mesh.getNodeName());

  page += "<div id='topbar'><h1>QR Join</h1><a href='/'>&#8592; Beheer</a></div>"
          "<div style='padding:10px'>";

  if (idx < 0 || idx >= MAX_ROOMS || !mesh.isRoomActive(idx)) {
    page += "<div class='card'><p class='err'>Room ";
    page += idx;
    page += " is niet actief.</p><p><a href='/'>&#8592; Terug</a></p></div>";
    page += FPSTR(HTML_FOOT);
    return page;
  }

  // Build 64-char hex public key
  char hex64[65] = {};
  const uint8_t* pub = mesh.getRoomPubKey(idx);
  for (int b = 0; b < PUB_KEY_SIZE; b++) {
    snprintf(hex64 + b * 2, 3, "%02x", (unsigned int)pub[b]);
  }

  // URL-encode room name for use in URI
  char enc_name[80] = {};
  const char* room_name = mesh.getRoomName(idx);
  int ei = 0;
  for (int ni = 0; room_name[ni] && ei < (int)sizeof(enc_name) - 4; ni++) {
    unsigned char c = (unsigned char)room_name[ni];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~') {
      enc_name[ei++] = (char)c;
    } else if (c == ' ') {
      enc_name[ei++] = '+';
    } else {
      snprintf(enc_name + ei, 4, "%%%02X", (unsigned int)c);
      ei += 3;
    }
  }
  enc_name[ei] = 0;

  // Build meshcore:// contact import URI (verified format from docs/qr_codes.md)
  // type=3 = ADV_TYPE_ROOM — room server contact
  String uri = "meshcore://contact/add?name=";
  uri += enc_name;
  uri += "&public_key=";
  uri += hex64;
  uri += "&type=3";

  page += "<div class='card'><h2>Join QR &mdash; ";
  page += room_name;
  page += "</h2>";

  // Generate QR code server-side using ricmoo/QRCode
  // Version 7 (45x45): holds up to 154 bytes at ECC_LOW — enough for our URI (~136 chars max)
  const int QR_VER = 7;
  QRCode qrcode;
  uint8_t qrData[qrcode_getBufferSize(QR_VER)];
  int qr_ok = qrcode_initText(&qrcode, qrData, QR_VER, ECC_LOW, uri.c_str());

  if (qr_ok != 0) {
    page += "<p class='err'>QR generation failed (URI may be too long).</p>";
  } else {
    // Pack module bits: 1 bit per module, MSB-first, row by row
    int sz = qrcode.size;
    int byte_count = (sz * sz + 7) / 8;
    uint8_t bits[256] = {};  // 254 bytes max for v7 45x45 QR
    for (int y = 0; y < sz; y++) {
      for (int x = 0; x < sz; x++) {
        int bit_idx = y * sz + x;
        if (qrcode_getModule(&qrcode, x, y)) {
          bits[bit_idx >> 3] |= (uint8_t)(0x80u >> (bit_idx & 7));
        }
      }
    }
    String b64 = base64Encode(bits, (size_t)byte_count);

    // Inline canvas renderer — all QR data is baked into the page, no external deps
    page += "<div style='text-align:center;margin:12px 0'><canvas id='q'></canvas></div>"
            "<script>(function(){"
            "var d='";
    page += b64;
    page += "',sz=";
    page += sz;
    page += ",s=6,p=4;"  // s=pixel scale, p=quiet-zone modules
            "var c=document.getElementById('q');"
            "c.width=c.height=(sz+p*2)*s;"
            "var ctx=c.getContext('2d');"
            "ctx.fillStyle='white';ctx.fillRect(0,0,c.width,c.height);"
            "ctx.fillStyle='black';"
            "var b=atob(d);"
            "for(var y=0;y<sz;y++)for(var x=0;x<sz;x++){"
            "var i=y*sz+x;"
            "if(b.charCodeAt(i>>3)&(0x80>>(i&7)))"
            "ctx.fillRect((x+p)*s,(y+p)*s,s,s);}"
            "})();</script>";
  }

  // URI as clickable text (on mobile, tapping may open companion app directly)
  page += "<p style='font-size:0.8em;word-break:break-all'>"
          "<b>URI:</b><br><a href='";
  page += uri;
  page += "'>";
  page += uri;
  page += "</a></p>";

  // Security notice
  page += "<p class='warn' style='font-size:0.85em'>&#9888; Anyone who scans this QR can add the room as a contact. ";
  if (mesh.getRoomGuestPassword(idx)[0] == 0) {
    page += "<b>No guest password set</b> &mdash; login is open to all.";
  } else {
    page += "Login still requires the room password.";
  }
  page += "</p>";

  if (mesh.isRoomStealth(idx)) {
    page += "<p style='font-size:0.85em;color:#aaa'>"
            "&#128272; Stealth ON &mdash; room is not advertising. "
            "This QR is the only out-of-band join path.</p>";
  }

  page += "<p><a href='/'>&#8592; Terug</a></p></div>"
          "</div>";  // padding wrapper
  page += FPSTR(HTML_FOOT);
  return page;
}

// ---------------------------------------------------------------------------
//  Routes
// ---------------------------------------------------------------------------
void WebManager::setupRoutes() {
  const char* user = "admin";
  const char* pass = _mesh.getAdminPassword();  // SEC-001: runtime password (randomised on first boot)

  // Main status page
  _server.on("/", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass))
      return req->requestAuthentication();
    String ip = (_mode == MODE_AP)
      ? WiFi.softAPIP().toString()
      : WiFi.localIP().toString();
    // Use chunked streaming to avoid a large contiguous heap allocation (JES-854).
    // AsyncResponseStream buffers in linked 1460-byte chunks, so a 40KB+ page never
    // requires more than ~2KB contiguous free heap at a time.
    AsyncResponseStream* stream =
      req->beginResponseStream("text/html; charset=utf-8");
    if (!stream) {
      req->send(503, "text/html; charset=utf-8",
        "<html><body style='background:#0f1117;color:#e0e0e0;font-family:sans-serif;padding:20px'>"
        "<h2 style='color:#ff4444'>Tijdelijk weinig geheugen</h2>"
        "<p>Pagina kon niet worden opgebouwd. Herlaad de pagina.</p>"
        "<p><a href='/' style='color:#00d4ff'>Opnieuw proberen</a></p>"
        "</body></html>");
      return;
    }
    buildStatusPageStream(*stream, ip.c_str());
    req->send(stream);
  });

  // API: set server (node) name (JES-828)
  _server.on("/api/node/name", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("name", true)) { req->send(400, "text/plain", "missing name"); return; }
      String n = req->getParam("name", true)->value();
      n.trim();
      if (!n.length() || n.length() > 31) {
        req->send(400, "text/plain", "name leeg of te lang (max 31 tekens)"); return;
      }
      char cmd[64], reply[160];
      snprintf(cmd, sizeof(cmd), "set name %.*s", 31, n.c_str());
      _mesh.handleCommand(0, cmd, reply);
      req->redirect("/");
    });

  // API: add room
  _server.on("/api/room/add", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      char reply[160] = {};
      _mesh.handleCommand(0, (char*)"room add", reply);
      req->redirect("/rooms");
    });

  // API: delete room
  _server.on("/api/room/del", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "missing idx"); return; }
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "room del %s",
               req->getParam("idx", true)->value().c_str());
      char reply[160] = {};
      _mesh.handleCommand(0, cmd, reply);
      req->redirect("/rooms");
    });

  // API: rekey room — generate new private key; double-confirm required
  _server.on("/api/room/rekey", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("idx", true) || !req->hasParam("confirm", true)) {
        req->send(400, "text/plain", "missing idx or confirm"); return;
      }
      int idx = req->getParam("idx", true)->value().toInt();
      if (idx < 0 || idx >= MAX_ROOMS || !_mesh.isRoomActive(idx)) {
        req->send(400, "text/plain", "invalid or inactive room"); return;
      }
      // Double-confirm: must type exact room name (room 0: "REKEY")
      String confirm_val = req->getParam("confirm", true)->value();
      String expected = (idx == 0) ? String("REKEY") : String(_mesh.getRoomName(idx));
      if (!confirm_val.equals(expected)) {
        req->send(400, "text/plain",
          "Bevestiging onjuist. Typ de exacte roomnaam ter bevestiging (of REKEY voor room 0).");
        return;
      }
      _mesh.rekeyRoom(idx);
      req->redirect("/rooms");
    });

  // API: set room fields
  _server.on("/api/room/set", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "missing idx"); return; }
      const String& idx = req->getParam("idx", true)->value();
      char cmd[160], reply[160];

      if (req->hasParam("name", true) && req->getParam("name", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s name %s", idx.c_str(),
                 req->getParam("name", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      if (req->hasParam("clear_pass", true) && req->getParam("clear_pass", true)->value() == "1") {
        snprintf(cmd, sizeof(cmd), "room set %s pass ", idx.c_str());
        _mesh.handleCommand(0, cmd, reply);
      } else if (req->hasParam("pass", true) && req->getParam("pass", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s pass %s", idx.c_str(),
                 req->getParam("pass", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      if (req->hasParam("clear_guest", true) && req->getParam("clear_guest", true)->value() == "1") {
        snprintf(cmd, sizeof(cmd), "room set %s guest ", idx.c_str());
        _mesh.handleCommand(0, cmd, reply);
      } else if (req->hasParam("guest", true) && req->getParam("guest", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s guest %s", idx.c_str(),
                 req->getParam("guest", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      req->redirect("/rooms");
    });

  // API: global stealth toggle (all rooms)
  _server.on("/api/stealth", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("stealth", true)) { req->send(400, "text/plain", "missing stealth"); return; }
      bool s = (req->getParam("stealth", true)->value() == "on");
      _mesh.setRoomStealth(-1, s);  // -1 = all rooms
      req->redirect("/rooms");
    });

  // API: repeater toggle (JES-855) — independent of stealth
  _server.on("/api/repeater", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("repeater", true)) { req->send(400, "text/plain", "missing repeater"); return; }
      char cmd[20], reply[80];
      bool on = (req->getParam("repeater", true)->value() == "on");
      snprintf(cmd, sizeof(cmd), "set fwd %s", on ? "on" : "off");
      _mesh.handleCommand(0, cmd, reply);
      req->redirect("/rooms");
    });

  // API: per-room stealth toggle
  _server.on("/api/room/stealth", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("idx", true) || !req->hasParam("stealth", true)) {
        req->send(400, "text/plain", "missing idx or stealth"); return;
      }
      int idx = req->getParam("idx", true)->value().toInt();
      bool s  = (req->getParam("stealth", true)->value() == "on");
      _mesh.setRoomStealth(idx, s);
      req->redirect("/rooms");
    });

  // API: set global advert interval (seconds)
  _server.on("/api/advert/interval", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("seconds", true)) { req->send(400, "text/plain", "missing seconds"); return; }
      int sec = req->getParam("seconds", true)->value().toInt();
      if (sec < 10 || sec > 3600) { req->send(400, "text/plain", "seconds must be 10-3600"); return; }
      _mesh.setAdvertIntervalSec((uint16_t)sec);
      req->redirect("/rooms");
    });

  // API: set anti-entropy sync interval (seconds) — JES-844
  _server.on("/api/sync/interval", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("seconds", true)) { req->send(400, "text/plain", "missing seconds"); return; }
      int sec = req->getParam("seconds", true)->value().toInt();
      if (sec < 10 || sec > 3600) { req->send(400, "text/plain", "seconds must be 10-3600"); return; }
      _mesh.setSyncIntervalSec((uint32_t)sec);
      req->redirect("/network");
    });

  // API: screensaver / Varia settings
  _server.on("/api/screensaver", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!_ui_task) { req->redirect("/system"); return; }
      if (req->hasParam("ss", true)) {
        _ui_task->setSsEnabled(req->getParam("ss", true)->value() == "on");
      }
      if (req->hasParam("keep", true)) {
        _ui_task->setSsKeepOn(req->getParam("keep", true)->value() == "on");
      }
      if (req->hasParam("interval", true)) {
        int sec = req->getParam("interval", true)->value().toInt();
        if (sec >= 1 && sec <= 60) _ui_task->setSsPageSec((uint8_t)sec);
      }
      req->redirect("/system");
    });

  // API: QR code page for a room
  _server.on("/api/room/qr", HTTP_GET,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("idx")) { req->send(400, "text/plain", "missing idx"); return; }
      int idx = req->getParam("idx")->value().toInt();
      req->send(200, "text/html; charset=utf-8", buildQrPage(_mesh, idx));
    });

  // API: switch WiFi mode
  _server.on("/api/wifi/mode", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("mode", true)) {
        const String& m = req->getParam("mode", true)->value();
        _mode = (m == "sta") ? MODE_STA : MODE_AP;
        saveConfig();
        req->redirect("/network");
        // Apply new mode after redirect is sent
        WiFi.disconnect(true);
        _connecting = false;
        if (_mode == MODE_AP) {
          startAP();
        } else {
          stopCaptivePortal();
          connectSTA();
        }
      } else {
        req->send(400, "text/plain", "missing mode");
      }
    });

  // API: update AP credentials
  _server.on("/api/wifi/ap", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("ssid", true) && req->getParam("ssid", true)->value().length()) {
        strncpy(_ap_ssid, req->getParam("ssid", true)->value().c_str(), sizeof(_ap_ssid) - 1);
        _ap_ssid[sizeof(_ap_ssid) - 1] = 0;
      }
      if (req->hasParam("pass", true)) {
        strncpy(_ap_pass, req->getParam("pass", true)->value().c_str(), sizeof(_ap_pass) - 1);
        _ap_pass[sizeof(_ap_pass) - 1] = 0;
      }
      saveConfig();
      req->redirect("/network");
      if (_mode == MODE_AP) {
        WiFi.softAPdisconnect(false);
        startAP();
      }
    });

  // API: update STA credentials and connect
  _server.on("/api/wifi/sta", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("ssid", true)) {
        strncpy(_sta_ssid, req->getParam("ssid", true)->value().c_str(), sizeof(_sta_ssid) - 1);
        _sta_ssid[sizeof(_sta_ssid) - 1] = 0;
      }
      if (req->hasParam("pass", true)) {
        strncpy(_sta_pass, req->getParam("pass", true)->value().c_str(), sizeof(_sta_pass) - 1);
        _sta_pass[sizeof(_sta_pass) - 1] = 0;
      }
      saveConfig();
      req->redirect("/network");
      if (_mode == MODE_STA) {
        WiFi.disconnect(false);
        _connecting = false;
        connectSTA();
      }
    });

  // API: set clock from browser timestamp (fallback when NTP unavailable)
  // POST /api/set_time  body: ts=<unix_seconds>
  // Admin-only; browser sends Math.floor(Date.now()/1000).
  _server.on("/api/set_time", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("ts", true)) {
        req->send(400, "application/json", "{\"error\":\"ts required\"}"); return;
      }
      long ts = req->getParam("ts", true)->value().toInt();
      if (ts < 1000000000L || ts > 2147483647L) {
        req->send(400, "application/json", "{\"error\":\"ts out of range\"}"); return;
      }
      _mesh.getRTCClock()->setCurrentTime((uint32_t)ts);
      saveClockEpoch();  // persist so next power cycle starts near correct time
      Serial.printf("[Clock] Time set via browser: %lu\n", (unsigned long)ts);
      req->send(200, "application/json", "{\"ok\":true}");
    });

  // POST /api/ntp  body: server=<hostname>
  // Save NTP server and (re)start SNTP sync (STA mode only).
  _server.on("/api/ntp", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!req->hasParam("server", true)) {
        req->send(400, "application/json", "{\"error\":\"server required\"}"); return;
      }
      String srv = req->getParam("server", true)->value();
      srv.trim();
      if (srv.length() == 0 || srv.length() > 63) {
        req->send(400, "application/json", "{\"error\":\"server invalid\"}"); return;
      }
      strncpy(_ntp_server, srv.c_str(), sizeof(_ntp_server) - 1);
      _ntp_server[sizeof(_ntp_server) - 1] = 0;
      saveConfig();
      // Restart SNTP with new server when in STA mode
      if (_mode == MODE_STA && WiFi.status() == WL_CONNECTED) {
        _ntp_synced = false;
        _ntp_check_ms = millis() + 1000;
        configTime(0, 0, _ntp_server, "time.cloudflare.com");
        Serial.printf("[NTP] Server updated to '%s', sync restarted\n", _ntp_server);
      }
      req->redirect("/network");
    });

  // API: backup download
  _server.on("/api/backup", HTTP_GET,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      String json = buildBackupJson();
      AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", json);
      resp->addHeader("Content-Disposition", "attachment; filename=\"siren-backup.json\"");
      resp->addHeader("Cache-Control", "no-store");
      req->send(resp);
    });

  // API: restore upload (multipart file upload)
  _server.on("/api/restore", HTTP_POST,
    // onRequest — called after all upload chunks are received
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (_restore_buf.length() == 0) {
        req->send(400, "text/plain", "No backup data received");
        return;
      }
      bool ok = applyRestore(_restore_buf);
      _restore_buf = "";
      if (ok) {
        String pg = buildHead(_mesh.getNodeName());
        pg += "<div class='card'><h2>Restore OK</h2>"
              "<p class='ok'>Settings applied. Rebooting in 2 seconds...</p>"
              "<p><a href='/'>Back</a></p></div>";
        pg += FPSTR(HTML_FOOT);
        req->send(200, "text/html; charset=utf-8", pg);
        delay(2000);
        ESP.restart();
      } else {
        String pg = buildHead(_mesh.getNodeName());
        pg += "<div class='card'><h2>Restore Failed</h2>"
              "<p class='err'>Invalid or incompatible backup file (version mismatch?).</p>"
              "<p><a href='/'>Back</a></p></div>";
        pg += FPSTR(HTML_FOOT);
        req->send(400, "text/html; charset=utf-8", pg);
      }
    },
    // onUpload — accumulate file chunks into _restore_buf
    [this](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) _restore_buf = "";
      if (_restore_buf.length() + len < 65536) {  // 64 KB cap (v2 backups include posts)
        _restore_buf += String((char*)data, len);
      }
    });

  // API: LoRa radio settings
  _server.on("/api/lora", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      char cmd[128], reply[160];
      // set radio <freq> <bw> <sf> <cr> — all four required; persists, reboot to apply
      if (req->hasParam("freq", true) && req->hasParam("bw", true) &&
          req->hasParam("sf", true)   && req->hasParam("cr", true)) {
        snprintf(cmd, sizeof(cmd), "set radio %s %s %s %s",
                 req->getParam("freq",    true)->value().c_str(),
                 req->getParam("bw",      true)->value().c_str(),
                 req->getParam("sf",      true)->value().c_str(),
                 req->getParam("cr",      true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      // set txpower <n> — applies live and persists
      if (req->hasParam("txpower", true)) {
        snprintf(cmd, sizeof(cmd), "set txpower %s",
                 req->getParam("txpower", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      req->redirect("/network");
    });

  // -------------------------------------------------------------------------
  // API: Debug log (JES-852)
  // -------------------------------------------------------------------------

  // GET /api/debuglog — JSON array of log entries (admin auth)
  _server.on("/api/debuglog", HTTP_GET,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      req->send(200, "application/json", buildDebugLogJson());
    });

  // POST /api/debuglog/enable?on=1|0 — enable or disable logging
  _server.on("/api/debuglog/enable", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("on", true)) {
        bool on = (req->getParam("on", true)->value() == "1");
        _mesh.enableDebugLog(on);
      }
      req->redirect("/system");
    });

  // POST /api/debuglog/clear — discard all log entries
  _server.on("/api/debuglog/clear", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      _mesh.clearDebugLog();
      req->redirect("/system");
    });

  // -------------------------------------------------------------------------
  // API: MeshCore scope / region settings (JES-852)
  // -------------------------------------------------------------------------

  // POST /api/meshcore — flood limits, forwarding, path_hash
  _server.on("/api/meshcore", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      char cmd[80], reply[160];

      auto param = [&](const char* n) -> const char* {
        return req->hasParam(n, true) ? req->getParam(n, true)->value().c_str() : nullptr;
      };

      const char* fwd = param("fwd");
      if (fwd) {
        snprintf(cmd, sizeof(cmd), "set fwd %s", (strcmp(fwd, "on") == 0) ? "on" : "off");
        _mesh.handleCommand(0, cmd, reply);
      }
      const char* fm = param("flood_max");
      if (fm && fm[0]) {
        snprintf(cmd, sizeof(cmd), "set flood_max %s", fm);
        _mesh.handleCommand(0, cmd, reply);
      }
      const char* fmu = param("flood_max_unscoped");
      if (fmu && fmu[0]) {
        snprintf(cmd, sizeof(cmd), "set flood_max_unscoped %s", fmu);
        _mesh.handleCommand(0, cmd, reply);
      }
      const char* fma = param("flood_max_advert");
      if (fma && fma[0]) {
        snprintf(cmd, sizeof(cmd), "set flood_max_advert %s", fma);
        _mesh.handleCommand(0, cmd, reply);
      }
      const char* phm = param("path_hash");
      if (phm && phm[0]) {
        snprintf(cmd, sizeof(cmd), "set path_hash %s", phm);
        _mesh.handleCommand(0, cmd, reply);
      }
      req->redirect("/network");
    });

  // POST /api/meshcore/region — set default scope region
  _server.on("/api/meshcore/region", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      char cmd[80], reply[160];
      if (req->hasParam("region", true)) {
        const String& rv = req->getParam("region", true)->value();
        if (rv == "null" || rv.isEmpty()) {
          snprintf(cmd, sizeof(cmd), "region default null");
        } else {
          // Validate: only allow alphanumeric + _ + - (region name chars)
          bool safe = true;
          for (size_t i = 0; i < rv.length(); i++) {
            char c = rv[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
              safe = false; break;
            }
          }
          if (safe && rv.length() < 30) {
            snprintf(cmd, sizeof(cmd), "region default %s", rv.c_str());
            _mesh.handleCommand(0, cmd, reply);
          }
        }
      }
      req->redirect("/network");
    });

  // API: MQTT enable/disable toggle (JES-792)
  _server.on("/api/mqtt/toggle", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!_mqtt_mgr) { req->redirect("/system"); return; }
      if (!req->hasParam("enable", true)) { req->send(400, "text/plain", "missing enable"); return; }
      char reply[160] = {};
      bool en = (req->getParam("enable", true)->value() == "on");
      _mqtt_mgr->handleMqttCommand(en ? "enable" : "disable", reply);
      req->redirect("/system");
    });

  // API: MQTT config form save (JES-792)
  // Password: if submitted empty, keep existing; if non-empty, update.
  _server.on("/api/mqtt/set", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!_mqtt_mgr) { req->redirect("/system"); return; }
      char reply[160] = {};
      char cmd[200];

      auto param = [&](const char* n) -> const char* {
        return req->hasParam(n, true) ? req->getParam(n, true)->value().c_str() : nullptr;
      };

      const char* host = param("host");
      if (host && host[0]) {
        snprintf(cmd, sizeof(cmd), "set host %s", host);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* port = param("port");
      if (port && port[0]) {
        snprintf(cmd, sizeof(cmd), "set port %s", port);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* tls_v = param("tls");
      if (tls_v) {
        snprintf(cmd, sizeof(cmd), "set tls %s", (strcmp(tls_v, "on") == 0) ? "on" : "off");
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* ca_fp = param("ca_fp");
      if (ca_fp) {
        snprintf(cmd, sizeof(cmd), "set ca_fp %s", ca_fp);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* mu = param("user");
      if (mu) {
        snprintf(cmd, sizeof(cmd), "set user %s", mu);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      // Password: only update if non-empty (write-only — never echoed)
      const char* mp = param("pass");
      if (mp && mp[0]) {
        snprintf(cmd, sizeof(cmd), "set pass %s", mp);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* cid = param("client_id");
      if (cid) {
        snprintf(cmd, sizeof(cmd), "set client_id %s", cid);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* nid = param("net_id");
      if (nid && nid[0]) {
        snprintf(cmd, sizeof(cmd), "set net_id %s", nid);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* qos_v = param("qos");
      if (qos_v) {
        snprintf(cmd, sizeof(cmd), "set qos %s", qos_v);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      const char* intv = param("interval");
      if (intv && intv[0]) {
        snprintf(cmd, sizeof(cmd), "set interval %s", intv);
        _mqtt_mgr->handleMqttCommand(cmd, reply);
      }
      req->redirect("/system");
    });

  // ---------------------------------------------------------------------------
  // IRC chat UI (JES-798) — all routes behind admin basic-auth
  // ---------------------------------------------------------------------------

  // GET /chat — IRC-style channel/messages/nicklist page
  _server.on("/chat", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    req->send(200, "text/html; charset=utf-8", buildChatPage());
  });

  // GET /api/chat/messages?room=<idx>&since=<ts>
  _server.on("/api/chat/messages", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();

    int room_idx = req->hasParam("room") ? req->getParam("room")->value().toInt() : 1;
    uint32_t since_ts = req->hasParam("since") ? (uint32_t)req->getParam("since")->value().toInt() : 0;

    if (room_idx <= 0 || room_idx >= MAX_ROOMS || !_mesh.isRoomActive(room_idx)) {
      req->send(400, "application/json", "[]"); return;  // JES-846: room 0 rejected
    }

    const PostInfo* pool = _mesh.getPostPool();
    // Collect posts for room, sorted ascending by timestamp
    const PostInfo* sorted[MAX_TOTAL_POSTS];
    int cnt = 0;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (pool[i].room_idx == (uint8_t)room_idx) sorted[cnt++] = &pool[i];
    }
    // Insertion sort by timestamp
    for (int i = 1; i < cnt; i++) {
      const PostInfo* k = sorted[i]; int j = i - 1;
      while (j >= 0 && sorted[j]->post_timestamp > k->post_timestamp) {
        sorted[j + 1] = sorted[j]; j--;
      }
      sorted[j + 1] = k;
    }

    String json = "[";
    bool first = true;
    for (int i = 0; i < cnt; i++) {
      // Only send posts newer than since_ts (strictly greater, or all if since==0)
      if (since_ts > 0 && sorted[i]->post_timestamp <= since_ts) continue;
      if (!first) json += ",";
      first = false;
      // origin_id as 8 hex chars for delete button (JES-824)
      char oid_hex[9] = {};
      for (int b = 0; b < 4; b++)
        snprintf(oid_hex + b * 2, 3, "%02x", (unsigned int)sorted[i]->origin_id[b]);
      json += "{\"ts\":";
      json += (unsigned long)sorted[i]->post_timestamp;
      json += ",\"origin_id\":\"";
      json += oid_hex;
      json += "\",\"author\":\"";
      json += jsonEscape(_mesh.resolveName(sorted[i]->author.pub_key));
      json += "\",\"text\":\"";
      json += jsonEscape(sorted[i]->text);
      json += "\"}";
    }
    json += "]";

    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", json);
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });

  // GET /api/chat/nicks?room=<idx>
  _server.on("/api/chat/nicks", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();

    int room_idx = req->hasParam("room") ? req->getParam("room")->value().toInt() : 1;
    if (room_idx <= 0 || room_idx >= MAX_ROOMS || !_mesh.isRoomActive(room_idx)) {
      req->send(400, "application/json", "[]"); return;  // JES-846: room 0 rejected
    }

    String json = _mesh.buildNickJson(room_idx);

    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", json);
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });

  // POST /api/chat/post — server-authored post (admin auth only)
  _server.on("/api/chat/post", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("room", true) || !req->hasParam("text", true)) {
      req->send(400, "text/plain", "missing room or text"); return;
    }
    int room_idx = req->getParam("room", true)->value().toInt();
    const String& text = req->getParam("text", true)->value();
    if (room_idx <= 0 || room_idx >= MAX_ROOMS || !_mesh.isRoomActive(room_idx)) {
      req->send(400, "application/json", "{\"error\":\"invalid room (use 1+, room 0 is identity-only)\"}"); return;  // JES-846
    }
    if (text.length() == 0) { req->send(400, "text/plain", "empty text"); return; }
    // Length clamp happens inside addServerPost -> addPost
    _mesh.addServerPost(room_idx, text.c_str());
    req->send(200, "text/plain", "OK");
  });

  // ---------------------------------------------------------------------------
  // DM (direct message) API (JES-808) — all routes behind admin basic-auth
  // ---------------------------------------------------------------------------

  // GET /api/dm/convs — JSON list of active DM conversations
  _server.on("/api/dm/convs", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    AsyncWebServerResponse* resp =
      req->beginResponse(200, "application/json", _mesh.buildDmConvsJson());
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });

  // GET /api/dm/thread?pub=XXXXXXXX — DM thread for one contact
  _server.on("/api/dm/thread", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("pub")) { req->send(400, "application/json", "[]"); return; }
    const String& pub = req->getParam("pub")->value();
    if (pub.length() != 8) { req->send(400, "application/json", "[]"); return; }
    AsyncWebServerResponse* resp =
      req->beginResponse(200, "application/json", _mesh.buildDmThreadJson(pub.c_str()));
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });

  // POST /api/dm/send (body: pub=XXXXXXXX&text=...) — send DM to a contact
  _server.on("/api/dm/send", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("pub", true) || !req->hasParam("text", true)) {
      req->send(400, "text/plain", "missing pub or text"); return;
    }
    const String& pub  = req->getParam("pub",  true)->value();
    const String& text = req->getParam("text", true)->value();
    if (pub.length() != 8)  { req->send(400, "text/plain", "invalid pub"); return; }
    if (text.length() == 0) { req->send(400, "text/plain", "empty text");  return; }
    bool ok = _mesh.dmSend(pub.c_str(), text.c_str());
    req->send(ok ? 200 : 404, "text/plain", ok ? "OK" : "contact not found");
  });

  // ---- ACL management (JES-720) — all routes behind admin basic-auth ----

  _server.on("/acl", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    req->send(200, "text/html; charset=utf-8", buildAclPage());
  });

  // GET /api/acl?room=<idx>  — JSON array of clients for that room
  _server.on("/api/acl", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("room")) { req->send(400, "application/json", "{\"error\":\"missing room\"}"); return; }
    int ridx = req->getParam("room")->value().toInt();
    if (ridx < 0 || ridx >= MAX_ROOMS || !_mesh.isRoomActive(ridx)) {
      req->send(404, "application/json", "{\"error\":\"room not found\"}"); return;
    }
    int nc = _mesh.getRoomNumClients(ridx);
    String j = "[";
    for (int c = 0; c < nc; c++) {
      const ClientInfo* ci = _mesh.getRoomClient(ridx, c);
      if (!ci) continue;
      char pub8[9] = {};
      for (int b = 0; b < 4; b++)
        snprintf(pub8 + b * 2, 3, "%02x", (unsigned int)ci->id.pub_key[b]);
      if (j.length() > 1) j += ",";
      j += "{\"pub\":\"";
      j += pub8;
      j += "\",\"name\":\"";
      j += jsonEscape(_mesh.resolveName(ci->id.pub_key));
      j += "\",\"perm\":";
      j += (ci->permissions & 3);
      j += "}";
    }
    j += "]";
    req->send(200, "application/json", j);
  });

  // POST /api/acl/set  — change permissions for a client
  _server.on("/api/acl/set", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("room", true) || !req->hasParam("pub", true) || !req->hasParam("perm", true)) {
      req->send(400, "text/plain", "missing params"); return;
    }
    int room = req->getParam("room", true)->value().toInt();
    String pub  = req->getParam("pub",  true)->value();
    int perm    = req->getParam("perm", true)->value().toInt();
    if (perm < 0 || perm > 3) { req->send(400, "text/plain", "invalid perm"); return; }
    bool ok = _mesh.setRoomClientPerm(room, pub.c_str(), (uint8_t)perm);
    if (ok) req->redirect("/acl");
    else    req->send(404, "text/plain", "client not found");
  });

  // ---- Peer management (JES-816) — all routes behind admin basic-auth ----

  // GET /api/peers — JSON list of configured peers + own pubkey
  _server.on("/api/peers", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    const uint8_t* own_pub = _mesh.getRoomPubKey(0);
    char own_hex[65] = {};
    if (own_pub) {
      for (int b = 0; b < PUB_KEY_SIZE; b++)
        snprintf(own_hex + b * 2, 3, "%02x", (unsigned int)own_pub[b]);
    }
    String json = "{\"own_pub\":\"";
    json += own_hex;
    json += "\",\"peers\":[";
    bool first = true;
    for (int i = 0; i < MAX_PEERS; i++) {
      const PeerInfo* p = _mesh.getPeer(i);
      if (!p || !p->active) continue;
      char pfx[9] = {};
      for (int b = 0; b < 4; b++)
        snprintf(pfx + b * 2, 3, "%02x", (unsigned int)p->pub_key[b]);
      if (!first) json += ",";
      first = false;
      json += "{\"idx\":";  json += i;
      json += ",\"name\":\""; json += jsonEscape(p->name); json += "\"";
      json += ",\"pub_prefix\":\""; json += pfx; json += "\"";
      json += ",\"last_contact\":"; json += (unsigned long)p->last_contact;
      json += "}";
    }
    json += "]}";
    req->send(200, "application/json", json);
  });

  // POST /api/peer/add (body: pub=<64hex>&name=<name>)
  _server.on("/api/peer/add", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
    if (!req->hasParam("pub", true)) { req->send(400, "text/plain", "missing pub"); return; }
    const String& pub_str = req->getParam("pub", true)->value();
    // Validate: must be exactly 64 hex characters
    if (pub_str.length() != 64) { req->send(400, "text/plain", "pub must be 64 hex chars"); return; }
    for (int i = 0; i < 64; i++) {
      char c = pub_str[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        req->send(400, "text/plain", "pub must be hex"); return;
      }
    }
    uint8_t key[PUB_KEY_SIZE] = {};
    if (!mesh::Utils::fromHex(key, PUB_KEY_SIZE, pub_str.c_str())) {
      req->send(400, "text/plain", "bad hex"); return;
    }
    // Name (optional, clamped to 23 chars)
    char name[24] = {};
    if (req->hasParam("name", true)) {
      const String& ns = req->getParam("name", true)->value();
      size_t nlen = ns.length();
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, ns.c_str(), nlen);
      name[nlen] = 0;
    }
    int idx = _mesh.addPeerFromWeb(key, name[0] ? name : nullptr);
    if (idx < 0) {
      req->send(409, "text/plain", "peer list full or duplicate"); return;
    }
    req->redirect("/network");
  });

  // POST /api/peer/del (body: idx=<n>)
  _server.on("/api/peer/del", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
    if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "missing idx"); return; }
    int idx = req->getParam("idx", true)->value().toInt();
    bool ok = _mesh.delPeerFromWeb(idx);
    if (!ok) { req->send(400, "text/plain", "invalid idx"); return; }
    req->redirect("/network");
  });

  // POST /api/room/delpost (body: room_idx=N&origin_id=XXXXXXXX&post_ts=T) — admin only (JES-824)
  _server.on("/api/room/delpost", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
    if (!req->hasParam("room_idx", true) || !req->hasParam("origin_id", true) ||
        !req->hasParam("post_ts", true)) {
      req->send(400, "application/json", "{\"error\":\"missing params\"}"); return;
    }
    int room_idx = req->getParam("room_idx", true)->value().toInt();
    if (room_idx < 0 || room_idx >= MAX_ROOMS || !_mesh.isRoomActive(room_idx)) {
      req->send(400, "application/json", "{\"error\":\"invalid room\"}"); return;
    }
    const String& oid_str = req->getParam("origin_id", true)->value();
    if (oid_str.length() != 8) {
      req->send(400, "application/json", "{\"error\":\"origin_id must be 8 hex chars\"}"); return;
    }
    for (int i = 0; i < 8; i++) {
      char c = oid_str[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        req->send(400, "application/json", "{\"error\":\"origin_id must be hex\"}"); return;
      }
    }
    uint8_t oid[4];
    for (int b = 0; b < 4; b++) {
      char hb[3] = { oid_str[b * 2], oid_str[b * 2 + 1], 0 };
      oid[b] = (uint8_t)strtoul(hb, nullptr, 16);
    }
    uint32_t post_ts = (uint32_t)req->getParam("post_ts", true)->value().toInt();
    if (post_ts == 0) {
      req->send(400, "application/json", "{\"error\":\"invalid post_ts\"}"); return;
    }
    bool found = _mesh.handleDeletePost((uint8_t)room_idx, oid, post_ts);
    if (found) {
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    }
  });

  // GET /api/room/notify?idx=N — list login-notification targets for a room (JES-834)
  _server.on("/api/room/notify", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!req->hasParam("idx")) { req->send(400, "application/json", "{\"error\":\"missing idx\"}"); return; }
    int idx = req->getParam("idx")->value().toInt();
    if (idx < 0 || idx >= MAX_ROOMS || !_mesh.isRoomActive(idx)) {
      req->send(400, "application/json", "{\"error\":\"invalid room\"}"); return;
    }
    String j = "{\"idx\":";
    j += idx;
    j += ",\"targets\":[";
    int cnt = _mesh.getNotifyTargetCount(idx);
    for (int i = 0; i < cnt; i++) {
      const uint8_t* k = _mesh.getNotifyTarget(idx, i);
      char hex[65] = {};
      for (int b = 0; b < PUB_KEY_SIZE; b++) {
        snprintf(hex + b * 2, 3, "%02x", k[b]);
      }
      if (i > 0) j += ",";
      j += "{\"pub_key\":\"";
      j += hex;
      j += "\",\"pub_prefix\":\"";
      j += String(hex).substring(0, 8);
      j += "\"}";
    }
    j += "]}";
    req->send(200, "application/json", j);
  });

  // POST /api/room/notify/add (body: idx=N&pubkey=<64hex>) — add notification target (JES-834)
  _server.on("/api/room/notify/add", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
    if (!req->hasParam("idx", true) || !req->hasParam("pubkey", true)) {
      req->send(400, "application/json", "{\"error\":\"missing idx or pubkey\"}"); return;
    }
    int idx = req->getParam("idx", true)->value().toInt();
    if (idx < 0 || idx >= MAX_ROOMS || !_mesh.isRoomActive(idx)) {
      req->send(400, "application/json", "{\"error\":\"invalid room\"}"); return;
    }
    const String& pub_str = req->getParam("pubkey", true)->value();
    if ((int)pub_str.length() != PUB_KEY_SIZE * 2) {
      req->send(400, "application/json", "{\"error\":\"pubkey must be 64 hex chars\"}"); return;
    }
    uint8_t key[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(key, PUB_KEY_SIZE, pub_str.c_str())) {
      req->send(400, "application/json", "{\"error\":\"invalid hex\"}"); return;
    }
    if (_mesh.addNotifyTarget(idx, key)) {
      req->redirect("/rooms");
    } else {
      req->send(400, "application/json", "{\"error\":\"target list full\"}");
    }
  });

  // POST /api/room/notify/del (body: idx=N&pubkey=<64hex>) — remove notification target (JES-834)
  _server.on("/api/room/notify/del", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
    if (!req->hasParam("idx", true) || !req->hasParam("pubkey", true)) {
      req->send(400, "application/json", "{\"error\":\"missing idx or pubkey\"}"); return;
    }
    int idx = req->getParam("idx", true)->value().toInt();
    if (idx < 0 || idx >= MAX_ROOMS || !_mesh.isRoomActive(idx)) {
      req->send(400, "application/json", "{\"error\":\"invalid room\"}"); return;
    }
    const String& pub_str = req->getParam("pubkey", true)->value();
    if ((int)pub_str.length() != PUB_KEY_SIZE * 2) {
      req->send(400, "application/json", "{\"error\":\"pubkey must be 64 hex chars\"}"); return;
    }
    uint8_t key[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(key, PUB_KEY_SIZE, pub_str.c_str())) {
      req->send(400, "application/json", "{\"error\":\"invalid hex\"}"); return;
    }
    _mesh.delNotifyTarget(idx, key);
    req->redirect("/rooms");
  });

  // POST /api/peer/sync (body: idx=<n> optional — omit for all peers)
  _server.on("/api/peer/sync", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    int idx = -1;  // -1 = all peers
    if (req->hasParam("idx", true) && req->getParam("idx", true)->value().length() > 0) {
      idx = req->getParam("idx", true)->value().toInt();
    }
    _mesh.triggerPeerSync(idx);
    req->redirect("/network");
  });

  // POST /api/peer/roomsync — push all local rooms to one or all peers (JES-848)
  _server.on("/api/peer/roomsync", HTTP_POST, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    int idx = -1;  // -1 = all peers
    if (req->hasParam("idx", true) && req->getParam("idx", true)->value().length() > 0) {
      idx = req->getParam("idx", true)->value().toInt();
    }
    _mesh.triggerRoomSync(idx);
    req->redirect("/network");
  });

  // GET /api/sync/status — sync diagnostics (JES-833, admin-auth required)
  // Returns JSON: global counters + per-room hash info + per-peer timestamps.
  // No private keys, passwords or message content in output.
  _server.on("/api/sync/status", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    String j = "{";
    // Global counters
    j += "\"counters\":{";
    j += "\"sync_req_sent\":";   j += _mesh.getSyncReqSent();   j += ",";
    j += "\"sync_dat_recv\":";   j += _mesh.getSyncDatRecv();   j += ",";
    j += "\"sync_posts_recv\":"; j += _mesh.getSyncPostsRecv(); j += ",";
    j += "\"sync_posts_sent\":"; j += _mesh.getSyncPostsSent(); j += "},";
    // Room hashes
    j += "\"rooms\":[";
    bool rf = true;
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!_mesh.isRoomActive(i)) continue;
      if (!rf) j += ",";
      rf = false;
      const uint8_t* pub = _mesh.getRoomPubKey(i);
      char hash[9] = {};
      if (pub) {
        for (int b = 0; b < 4; b++) snprintf(hash + b * 2, 3, "%02x", (unsigned int)pub[b]);
      }
      j += "{\"idx\":";      j += i;
      j += ",\"name\":\"";   j += jsonEscape(_mesh.getRoomName(i)); j += "\"";
      j += ",\"hash\":\"";   j += hash; j += "\"}";
    }
    j += "],";
    // Per-peer sync state
    j += "\"peers\":[";
    bool pf = true;
    for (int i = 0; i < MAX_PEERS; i++) {
      const PeerInfo* p = _mesh.getPeer(i);
      if (!p || !p->active) continue;
      if (!pf) j += ",";
      pf = false;
      char pfx[9] = {};
      for (int b = 0; b < 4; b++) snprintf(pfx + b * 2, 3, "%02x", (unsigned int)p->pub_key[b]);
      // Derive status string (no user-controlled content)
      const char* status;
      if (p->last_syncend_ts > 0 || p->last_syncdat_ts > 0) status = "OK";
      else if (p->last_syncreq_ts > 0)                       status = "geen_response";
      else                                                    status = "wacht";
      j += "{\"idx\":";             j += i;
      j += ",\"name\":\"";          j += jsonEscape(p->name); j += "\"";
      j += ",\"pub_prefix\":\"";    j += pfx; j += "\"";
      j += ",\"last_syncreq_ts\":"; j += (unsigned long)p->last_syncreq_ts;
      j += ",\"last_syncdat_ts\":"; j += (unsigned long)p->last_syncdat_ts;
      j += ",\"last_syncend_ts\":"; j += (unsigned long)p->last_syncend_ts;
      j += ",\"sync_posts_recv\":"; j += (unsigned long)p->sync_posts_recv;
      j += ",\"sync_posts_sent\":"; j += (unsigned long)p->sync_posts_sent;
      j += ",\"status\":\"";        j += status; j += "\"";
      j += "}";
    }
    j += "]}";
    req->send(200, "application/json", j);
  });

  // GET /stats — statistics page (JES-800)
  _server.on("/stats", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    req->send(200, "text/html; charset=utf-8", buildStatsPage());
  });

  // GET /rooms — Room management page (JES-854 split)
  _server.on("/rooms", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    AsyncResponseStream* stream = req->beginResponseStream("text/html; charset=utf-8");
    if (!stream) { req->send(503, "text/plain", "OOM"); return; }
    buildRoomsPageStream(*stream);
    req->send(stream);
  });

  // GET /network — Network settings page (JES-854 split)
  _server.on("/network", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    String ip = (_mode == MODE_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    AsyncResponseStream* stream = req->beginResponseStream("text/html; charset=utf-8");
    if (!stream) { req->send(503, "text/plain", "OOM"); return; }
    buildNetworkPageStream(*stream, ip.c_str());
    req->send(stream);
  });

  // GET /system — System settings page (JES-854 split)
  _server.on("/system", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    AsyncResponseStream* stream = req->beginResponseStream("text/html; charset=utf-8");
    if (!stream) { req->send(503, "text/plain", "OOM"); return; }
    buildSystemPageStream(*stream);
    req->send(stream);
  });

  // GET /api/stats — statistics JSON (JES-800)
  _server.on("/api/stats", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass)) return req->requestAuthentication();
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", buildStatsJson());
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });

  // ---------------------------------------------------------------------------
  // OTA self-update (JES-774)
  // ---------------------------------------------------------------------------

  // POST /api/ota/check — trigger async version-manifest check
  _server.on("/api/ota/check", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      _ota_mgr.startCheck();
      req->redirect("/system");
    });

  // POST /api/ota/update — trigger async OTA download+flash
  // Only effective when an update is available (OtaManager enforces this).
  _server.on("/api/ota/update", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!checkOrigin(req)) { req->send(403, "text/plain", "CSRF check failed"); return; }
      if (!_ota_mgr.startUpdate()) {
        req->send(400, "text/plain",
          "Geen update beschikbaar — voer eerst 'Controleer op update' uit");
        return;
      }
      req->redirect("/system");
    });

  // GET /api/ota/status — JSON status (progress, state, versions)
  _server.on("/api/ota/status", HTTP_GET,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      String j = "{\"current_version\":\"" FIRMWARE_VERSION "\""
                 ",\"hw_target\":\"" SIREN_HARDWARE_TARGET "\"";
      j += ",\"available_version\":\"";
      j += jsonEscape(_ota_mgr.getAvailableVersion());
      j += "\",\"state\":";
      j += (int)_ota_mgr.getState();
      j += ",\"progress\":";
      j += _ota_mgr.getProgress();
      j += ",\"error\":\"";
      j += jsonEscape(_ota_mgr.getErrorMsg());
      j += "\"}";
      AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", j);
      resp->addHeader("Cache-Control", "no-store");
      req->send(resp);
    });

  // ---------------------------------------------------------------------------
  // Captive portal detection — respond to OS probes so device opens the UI
  // automatically. Android expects 204 on /generate_204; Apple/Windows want
  // a small success page. Redirect everything else to the management root.
  // These routes intentionally require NO auth (the redirect leads to auth).
  // ---------------------------------------------------------------------------
  auto cpRedirect = [](AsyncWebServerRequest* req) {
    req->redirect("http://192.168.4.1/");
  };
  // Android connectivity check
  _server.on("/generate_204",        HTTP_GET, cpRedirect);
  _server.on("/gen_204",             HTTP_GET, cpRedirect);
  // Apple CNA
  _server.on("/hotspot-detect.html", HTTP_GET, cpRedirect);
  _server.on("/library/test/success.html", HTTP_GET, cpRedirect);
  // Windows NCSI
  _server.on("/ncsi.txt",            HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Microsoft NCSI");
  });
  _server.on("/connecttest.txt",     HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Microsoft Connect Test");
  });
  _server.on("/redirect",            HTTP_GET, cpRedirect);
  _server.on("/canonical.html",      HTTP_GET, cpRedirect);

  // Catch-all: redirect unknown paths to management UI (captive portal UX)
  _server.onNotFound([this](AsyncWebServerRequest* req) {
    if (_mode == MODE_AP) {
      req->redirect("http://192.168.4.1/");
    } else {
      req->send(404, "text/plain", "Not Found");
    }
  });

  // OTA via ElegantOTA
  char ota_id[80];
  snprintf(ota_id, sizeof(ota_id), "%s (%s)", _mesh.getNodeName(), "SIREN Room Server");
  AsyncElegantOTA.setID(ota_id);
  AsyncElegantOTA.begin(&_server, user, pass);

  _server.begin();
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
void WebManager::begin() {
  loadConfig();
  loadClockEpoch();

#if defined(SIREN_DEFAULT_STA_SSID)
  // Provisioning: if no STA credentials configured yet, apply build-time defaults.
  if (_sta_ssid[0] == 0) {
    strncpy(_sta_ssid, SIREN_DEFAULT_STA_SSID, sizeof(_sta_ssid) - 1);
    _sta_ssid[sizeof(_sta_ssid) - 1] = 0;
#if defined(SIREN_DEFAULT_STA_PASS)
    strncpy(_sta_pass, SIREN_DEFAULT_STA_PASS, sizeof(_sta_pass) - 1);
    _sta_pass[sizeof(_sta_pass) - 1] = 0;
#endif
    _mode = MODE_STA;
    saveConfig();
    Serial.printf("[WiFi] Provisioned STA from build defaults: '%s'\n", _sta_ssid);
  }
#endif

  // Ensure AP SSID is set (use node name if default/empty after config load)
  if (_ap_ssid[0] == 0) {
    strncpy(_ap_ssid, _mesh.getNodeName(), sizeof(_ap_ssid) - 1);
    _ap_ssid[sizeof(_ap_ssid) - 1] = 0;
    // Prefix with "SIREN-" if not already
    if (strncmp(_ap_ssid, "SIREN-", 6) != 0) {
      char tmp[64];
      strncpy(tmp, _ap_ssid, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = 0;
      snprintf(_ap_ssid, sizeof(_ap_ssid), "SIREN-%s", tmp);
    }
  }

  if (_mode == MODE_AP) {
    startAP();
  } else {
    connectSTA();
  }
}

void WebManager::loop() {
  AsyncElegantOTA.loop();

  if (_dns_started) {
    _dns.processNextRequest();
  }

  // NTP clock sync: poll SNTP status every 2 s until the RTC is set,
  // then re-sync once every hour to correct drift.
  if (_mode == MODE_STA && !_connecting && WiFi.status() == WL_CONNECTED
      && millis() >= _ntp_check_ms) {
    _ntp_check_ms = millis() + (_ntp_synced ? 3600000UL : 2000UL);
    struct tm ti{};
    if (getLocalTime(&ti, 0)) {
      time_t now = mktime(&ti);
      if (now > 1000000000L) {
        _mesh.getRTCClock()->setCurrentTime((uint32_t)now);
        if (!_ntp_synced) {
          _ntp_synced = true;
          Serial.printf("[NTP] Clock synced — %s", asctime(&ti));
          saveClockEpoch();  // persist so AP-mode reboots start with correct time
        }
      }
    }
  }

  if (_mode == MODE_STA && _connecting) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      _connecting = false;
      Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
      if (!_started) {
        setupRoutes();
        _started = true;
        Serial.printf("[WiFi] Web UI: http://%s/\n", WiFi.localIP().toString().c_str());
      }
      // Start NTP sync now that we have internet access
      _ntp_synced = false;
      _ntp_check_ms = millis() + 2000;  // first check after 2 s
      configTime(0, 0, _ntp_server, "time.cloudflare.com");
      Serial.printf("[NTP] Sync started — server: %s\n", _ntp_server);
    } else if (millis() - _connect_started > WIFI_CONNECT_TIMEOUT_MS) {
      _connecting = false;
      Serial.printf("[WiFi] STA connect timeout (status %d). Falling back to AP mode.\n", st);
      _mode = MODE_AP;
      startAP();
    }
  }
}

// ---------------------------------------------------------------------------
//  CLI command handler
// ---------------------------------------------------------------------------
bool WebManager::handleWifiCommand(const char* args, char* reply) {
  while (*args == ' ') args++;

  // wifi mode ap|sta
  if (memcmp(args, "mode ", 5) == 0) {
    const char* m = args + 5;
    while (*m == ' ') m++;
    if (strcmp(m, "ap") == 0) {
      _mode = MODE_AP;
      saveConfig();
      WiFi.disconnect(true);
      _connecting = false;
      startAP();
      strcpy(reply, "OK - switched to AP mode");
    } else if (strcmp(m, "sta") == 0) {
      _mode = MODE_STA;
      saveConfig();
      stopCaptivePortal();
      WiFi.softAPdisconnect(false);
      connectSTA();
      strcpy(reply, "OK - switched to STA mode, connecting...");
    } else {
      strcpy(reply, "Err - usage: wifi mode ap|sta");
    }
    return true;
  }

  // wifi ap ssid <name>
  if (memcmp(args, "ap ssid ", 8) == 0) {
    strncpy(_ap_ssid, args + 8, sizeof(_ap_ssid) - 1);
    _ap_ssid[sizeof(_ap_ssid) - 1] = 0;
    saveConfig();
    sprintf(reply, "OK - AP SSID set to '%s'", _ap_ssid);
    return true;
  }

  // wifi ap pass <pass>
  if (memcmp(args, "ap pass ", 8) == 0) {
    strncpy(_ap_pass, args + 8, sizeof(_ap_pass) - 1);
    _ap_pass[sizeof(_ap_pass) - 1] = 0;
    saveConfig();
    strcpy(reply, "OK - AP password saved");
    return true;
  }

  // wifi ssid <name>  (STA)
  if (memcmp(args, "ssid ", 5) == 0) {
    strncpy(_sta_ssid, args + 5, sizeof(_sta_ssid) - 1);
    _sta_ssid[sizeof(_sta_ssid) - 1] = 0;
    saveConfig();
    sprintf(reply, "OK - STA SSID set to '%s' (use 'wifi connect' to connect)", _sta_ssid);
    return true;
  }

  // wifi pass <pass>  (STA)
  if (memcmp(args, "pass ", 5) == 0) {
    strncpy(_sta_pass, args + 5, sizeof(_sta_pass) - 1);
    _sta_pass[sizeof(_sta_pass) - 1] = 0;
    saveConfig();
    strcpy(reply, "OK - STA password saved");
    return true;
  }

  // wifi connect  (STA)
  if (strcmp(args, "connect") == 0) {
    if (_sta_ssid[0] == 0) {
      strcpy(reply, "Err - no STA SSID set (use 'wifi ssid <name>')");
      return true;
    }
    _mode = MODE_STA;
    saveConfig();
    WiFi.disconnect(false);
    _connecting = false;
    connectSTA();
    sprintf(reply, "OK - connecting to '%s'...", _sta_ssid);
    return true;
  }

  // wifi status
  if (strcmp(args, "status") == 0) {
    if (_mode == MODE_AP) {
      sprintf(reply, "AP mode — SSID='%s' IP=%s clients=%d",
              _ap_ssid,
              WiFi.softAPIP().toString().c_str(),
              WiFi.softAPgetStationNum());
    } else if (WiFi.status() == WL_CONNECTED) {
      sprintf(reply, "STA connected — SSID='%s' IP=%s",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else if (_connecting) {
      sprintf(reply, "STA connecting to '%s'...", _sta_ssid);
    } else {
      sprintf(reply, "STA disconnected (SSID='%s')", _sta_ssid);
    }
    return true;
  }

  strcpy(reply, "Err - usage: wifi mode ap|sta | wifi ap ssid <n> | wifi ap pass <p>"
                " | wifi ssid <n> | wifi pass <p> | wifi connect | wifi status");
  return true;
}

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
