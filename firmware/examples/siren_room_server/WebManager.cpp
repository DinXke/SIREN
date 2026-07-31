#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include "WebManager.h"
#include <AsyncElegantOTA.h>
#include <qrcode.h>

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

// Default AP SSID prefix — node name is appended at runtime
#define AP_SSID_PREFIX "SIREN-"

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------
WebManager::WebManager(MultiRoomMesh& mesh)
  : _server(80), _mesh(mesh), _started(false), _dns_started(false),
    _mode(MODE_AP),
    _connect_started(0), _connecting(false)
{
  // Default AP SSID: "SIREN-Node" — overwritten in begin() with real node name
  strncpy(_ap_ssid, "SIREN-Node", sizeof(_ap_ssid) - 1);
  _ap_ssid[sizeof(_ap_ssid) - 1] = 0;
  _ap_pass[0]  = 0;
  _sta_ssid[0] = 0;
  _sta_pass[0] = 0;
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

  extractField("ap_ssid",  _ap_ssid,  sizeof(_ap_ssid));
  extractField("ap_pass",  _ap_pass,  sizeof(_ap_pass));
  extractField("sta_ssid", _sta_ssid, sizeof(_sta_ssid));
  extractField("sta_pass", _sta_pass, sizeof(_sta_pass));
}

void WebManager::saveConfig() {
  File f = SPIFFS.open(WIFI_CONFIG_PATH, "w");
  if (!f) return;
  f.printf("{\"mode\":\"%s\","
           "\"ap_ssid\":\"%s\",\"ap_pass\":\"%s\","
           "\"sta_ssid\":\"%s\",\"sta_pass\":\"%s\"}",
           _mode == MODE_STA ? "sta" : "ap",
           _ap_ssid, _ap_pass,
           _sta_ssid, _sta_pass);
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

// ---------------------------------------------------------------------------
//  Backup JSON builder
// ---------------------------------------------------------------------------
String WebManager::buildBackupJson() {
  const NodePrefs* p = _mesh.getNodePrefs();

  String j = "{";
  j += "\"version\":\"1\",";
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
  if (!extractField("version", ver, sizeof(ver)) || strcmp(ver, "1") != 0) return false;

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
    snprintf(cmd, sizeof(cmd), "set pass %s", val);
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
//  HTML page builders
// ---------------------------------------------------------------------------
static const char HTML_HEAD[] PROGMEM =
  "<!DOCTYPE html><html><head>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>SIREN Room Server</title>"
  "<style>"
  "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:12px;}"
  "h2{color:#00d4ff;margin:0 0 8px}"
  "table{border-collapse:collapse;width:100%;margin-bottom:12px}"
  "th,td{border:1px solid #333;padding:4px 8px;text-align:left}"
  "th{background:#2a2a4a;color:#00d4ff}"
  "form{margin-top:8px}"
  "input,select{background:#2a2a4a;border:1px solid #555;color:#e0e0e0;padding:4px;margin:2px}"
  "button{background:#00d4ff;border:none;color:#000;padding:5px 12px;cursor:pointer;margin:2px}"
  ".card{background:#16213e;border:1px solid #333;padding:10px;margin-bottom:10px;border-radius:4px}"
  ".ok{color:#00ff88}.err{color:#ff4444}.warn{color:#ffcc00}"
  "a{color:#00d4ff}"
  "</style></head><body>";

static const char HTML_FOOT[] PROGMEM = "</body></html>";

String WebManager::buildStatusPage(MultiRoomMesh& mesh, const char* ip,
                                    WifiMode mode,
                                    const char* ap_ssid, const char* sta_ssid) {
  String page = FPSTR(HTML_HEAD);

  // Node info
  page += "<div class='card'><h2>SIREN Room Server</h2>";
  page += "<table>";
  page += "<tr><th>Node</th><td>"; page += mesh.getNodeName(); page += "</td></tr>";
  page += "<tr><th>WiFi Mode</th><td>"; page += (mode == MODE_AP ? "AP (hotspot)" : "STA (client)"); page += "</td></tr>";
  page += "<tr><th>IP</th><td>"; page += ip; page += "</td></tr>";
  page += "<tr><th>Firmware</th><td>" FIRMWARE_VERSION " (" FIRMWARE_BUILD_DATE ")</td></tr>";
  page += "<tr><th>Active Rooms</th><td>"; page += mesh.getNumActiveRooms();
  page += " / "; page += MAX_ROOMS; page += "</td></tr>";
  page += "</table></div>";

  // Rooms table
  page += "<div class='card'><h2>Rooms</h2>";
  page += "<table><tr><th>#</th><th>Name</th><th>Stealth</th><th>Clients</th><th>Posts</th><th>Actions</th></tr>";
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!mesh.isRoomActive(i)) continue;
    bool stealth_i = mesh.isRoomStealth(i);
    page += "<tr><td>"; page += i;
    page += "</td><td>"; page += mesh.getRoomName(i);
    page += "</td><td>";
    page += stealth_i ? "<span class='warn'>STEALTH</span>" : "<span class='ok'>VISIBLE</span>";
    page += "</td><td>"; page += mesh.getRoomClientCount(i);
    page += "</td><td>"; page += mesh.getRoomPostCount(i);
    page += "</td><td>";
    // Per-room stealth toggle
    page += "<form method='post' action='/api/room/stealth' style='display:inline'>"
            "<input type='hidden' name='idx' value='"; page += i;
    page += "'><input type='hidden' name='stealth' value='";
    page += stealth_i ? "off" : "on";
    page += "'><button type='submit'>";
    page += stealth_i ? "Make Visible" : "Hide";
    page += "</button></form>";
    // QR join code button
    page += " <a href='/api/room/qr?idx="; page += i;
    page += "' style='text-decoration:none'><button>QR</button></a>";
    if (i > 0) {
      page += " <form method='post' action='/api/room/del' style='display:inline'>"
              "<input type='hidden' name='idx' value='"; page += i;
      page += "'><button onclick=\"return confirm('Delete room "; page += i;
      page += "?')\">Del</button></form>";
    }
    page += "</td></tr>";
  }
  page += "</table>";
  page += "<form method='post' action='/api/room/add'>"
          "<button type='submit'>+ Add Room</button></form></div>";

  // Edit room form
  page += "<div class='card'><h2>Edit Room</h2>"
          "<form method='post' action='/api/room/set'>"
          "Idx: <input name='idx' size='3'> "
          "Name: <input name='name' size='20'> "
          "Pass: <input name='pass' size='16'> "
          "Guest: <input name='guest' size='16'> "
          "<button type='submit'>Save</button></form></div>";

  // Visibility (stealth) — global toggle
  {
    bool all_stealth = true;
    bool any_active  = false;
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!mesh.isRoomActive(i)) continue;
      any_active = true;
      if (!mesh.isRoomStealth(i)) { all_stealth = false; break; }
    }
    page += "<div class='card'><h2>Visibility</h2>";
    if (any_active && all_stealth) {
      page += "<p>All rooms: <b class='warn'>STEALTH</b> — no adverts sent. "
              "Rooms are joinable if you know the key (QR / out-of-band).</p>";
    } else {
      page += "<p>One or more rooms: <b class='ok'>VISIBLE</b> — adverts active.</p>";
    }
    page += "<form method='post' action='/api/stealth'>"
            "<button name='stealth' value='off'>Make All Visible</button> "
            "<button name='stealth' value='on' onclick=\"return confirm('Hide all rooms? They stop broadcasting adverts.')\">Hide All (Stealth)</button>"
            "</form>"
            "<p style='font-size:0.85em;color:#aaa'>Per-room: use the toggle buttons in the Rooms table above, "
            "or CLI: <code>room stealth &lt;idx&gt; on|off</code></p>"
            "</div>";
  }

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

    page += "<form method='post' action='/api/lora'>"
            "Freq (MHz): <input name='freq' value='"; page += freq_str;
    page += "' size='10'> "
            "BW (kHz): <select name='bw'>";

    static const float BW_OPTS[]   = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
    static const char* BW_LABELS[] = {"7.80","10.40","15.60","20.80","31.25","41.70","62.50","125.00","250.00","500.00"};
    for (int i = 0; i < 10; i++) {
      page += "<option value='"; page += BW_LABELS[i];
      if (fabsf(BW_OPTS[i] - p->bw) < 0.5f) page += "' selected='selected";
      page += "'>"; page += BW_LABELS[i]; page += "</option>";
    }
    page += "</select> "
            "SF: <select name='sf'>";
    for (int sf = 5; sf <= 12; sf++) {
      page += "<option value='"; page += sf;
      if (sf == (int)p->sf) page += "' selected='selected";
      page += "'>SF"; page += sf; page += "</option>";
    }
    page += "</select> "
            "CR: <select name='cr'>";
    for (int cr = 5; cr <= 8; cr++) {
      page += "<option value='"; page += cr;
      if (cr == (int)p->cr) page += "' selected='selected";
      page += "'>4/"; page += cr; page += "</option>";
    }
    page += "</select> "
            "TX (dBm): <input name='txpower' value='"; page += tx_str;
    page += "' size='4' type='number' min='2' max='22'> "
            "<button type='submit'>Save LoRa</button></form>"
            "<p style='font-size:0.85em;color:#aaa'>Freq/BW/SF/CR changes require reboot to apply. TX power is live.</p>"
            "</div>";
  }

  // WiFi config
  page += "<div class='card'><h2>WiFi</h2>";
  page += "<form method='post' action='/api/wifi/mode'>"
          "Mode: <select name='mode'>"
          "<option value='ap'"; page += (mode == MODE_AP ? " selected" : ""); page += ">AP (own hotspot)</option>"
          "<option value='sta'"; page += (mode == MODE_STA ? " selected" : ""); page += ">STA (connect to network)</option>"
          "</select> <button type='submit'>Switch Mode</button></form>";

  page += "<br><b>AP Settings</b>"
          "<form method='post' action='/api/wifi/ap'>"
          "SSID: <input name='ssid' value='"; page += ap_ssid;
  page += "' size='24'> "
          "Password: <input name='pass' type='password' size='24' placeholder='empty=open'> "
          "<button type='submit'>Save AP</button></form>";

  page += "<br><b>STA Settings</b>"
          "<form method='post' action='/api/wifi/sta'>"
          "SSID: <input name='ssid' value='"; page += sta_ssid;
  page += "' size='24'> "
          "Password: <input name='pass' type='password' size='24'> "
          "<button type='submit'>Save STA &amp; Connect</button></form>"
          "<p>Current IP: <b>"; page += ip;
  page += "</b></p></div>";

  // Backup / Restore
  page += "<div class='card'><h2>Backup &amp; Restore</h2>";
  page += "<p><a href='/api/backup'><button>Download Backup</button></a> "
          "— exports all settings + private keys as JSON</p>";
  page += "<form method='post' action='/api/restore' enctype='multipart/form-data'>"
          "Restore: <input type='file' name='backup' accept='.json'> "
          "<button type='submit' onclick=\"return confirm('Restore will overwrite all settings and reboot. Continue?')\">Restore &amp; Reboot</button>"
          "</form></div>";

  // OTA link
  page += "<div class='card'><a href='/update'>OTA Firmware Update</a></div>";

  page += FPSTR(HTML_FOOT);
  return page;
}

// ---------------------------------------------------------------------------
//  QR code page — per-room join QR rendered via inline canvas JS
// ---------------------------------------------------------------------------
static String buildQrPage(MultiRoomMesh& mesh, int idx) {
  String page = FPSTR(HTML_HEAD);

  if (idx < 0 || idx >= MAX_ROOMS || !mesh.isRoomActive(idx)) {
    page += "<div class='card'><p class='err'>Room ";
    page += idx;
    page += " is not active.</p><p><a href='/'>&#8592; Back</a></p></div>";
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

  page += "<p><a href='/'>&#8592; Back</a></p></div>";
  page += FPSTR(HTML_FOOT);
  return page;
}

// ---------------------------------------------------------------------------
//  Routes
// ---------------------------------------------------------------------------
void WebManager::setupRoutes() {
  const char* user = "admin";
  const char* pass = ADMIN_PASSWORD;

  // Main status page
  _server.on("/", HTTP_GET, [this, user, pass](AsyncWebServerRequest* req) {
    if (!req->authenticate(user, pass))
      return req->requestAuthentication();
    String ip = (_mode == MODE_AP)
      ? WiFi.softAPIP().toString()
      : WiFi.localIP().toString();
    req->send(200, "text/html",
              buildStatusPage(_mesh, ip.c_str(), _mode, _ap_ssid, _sta_ssid));
  });

  // API: add room
  _server.on("/api/room/add", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      char reply[160] = {};
      _mesh.handleCommand(0, (char*)"room add", reply);
      req->redirect("/");
    });

  // API: delete room
  _server.on("/api/room/del", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "missing idx"); return; }
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "room del %s",
               req->getParam("idx", true)->value().c_str());
      char reply[160] = {};
      _mesh.handleCommand(0, cmd, reply);
      req->redirect("/");
    });

  // API: set room fields
  _server.on("/api/room/set", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "missing idx"); return; }
      const String& idx = req->getParam("idx", true)->value();
      char cmd[160], reply[160];

      if (req->hasParam("name", true) && req->getParam("name", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s name %s", idx.c_str(),
                 req->getParam("name", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      if (req->hasParam("pass", true) && req->getParam("pass", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s pass %s", idx.c_str(),
                 req->getParam("pass", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      if (req->hasParam("guest", true) && req->getParam("guest", true)->value().length()) {
        snprintf(cmd, sizeof(cmd), "room set %s guest %s", idx.c_str(),
                 req->getParam("guest", true)->value().c_str());
        _mesh.handleCommand(0, cmd, reply);
      }
      req->redirect("/");
    });

  // API: global stealth toggle (all rooms)
  _server.on("/api/stealth", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("stealth", true)) { req->send(400, "text/plain", "missing stealth"); return; }
      bool s = (req->getParam("stealth", true)->value() == "on");
      _mesh.setRoomStealth(-1, s);  // -1 = all rooms
      req->redirect("/");
    });

  // API: per-room stealth toggle
  _server.on("/api/room/stealth", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("idx", true) || !req->hasParam("stealth", true)) {
        req->send(400, "text/plain", "missing idx or stealth"); return;
      }
      int idx = req->getParam("idx", true)->value().toInt();
      bool s  = (req->getParam("stealth", true)->value() == "on");
      _mesh.setRoomStealth(idx, s);
      req->redirect("/");
    });

  // API: QR code page for a room
  _server.on("/api/room/qr", HTTP_GET,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (!req->hasParam("idx")) { req->send(400, "text/plain", "missing idx"); return; }
      int idx = req->getParam("idx")->value().toInt();
      req->send(200, "text/html", buildQrPage(_mesh, idx));
    });

  // API: switch WiFi mode
  _server.on("/api/wifi/mode", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("mode", true)) {
        const String& m = req->getParam("mode", true)->value();
        _mode = (m == "sta") ? MODE_STA : MODE_AP;
        saveConfig();
        req->redirect("/");
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
      req->redirect("/");
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
      req->redirect("/");
      if (_mode == MODE_STA) {
        WiFi.disconnect(false);
        _connecting = false;
        connectSTA();
      }
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
      if (_restore_buf.length() == 0) {
        req->send(400, "text/plain", "No backup data received");
        return;
      }
      bool ok = applyRestore(_restore_buf);
      _restore_buf = "";
      if (ok) {
        String pg = FPSTR(HTML_HEAD);
        pg += "<div class='card'><h2>Restore OK</h2>"
              "<p class='ok'>Settings applied. Rebooting in 2 seconds...</p>"
              "<p><a href='/'>Back</a></p></div>";
        pg += FPSTR(HTML_FOOT);
        req->send(200, "text/html", pg);
        delay(2000);
        ESP.restart();
      } else {
        String pg = FPSTR(HTML_HEAD);
        pg += "<div class='card'><h2>Restore Failed</h2>"
              "<p class='err'>Invalid or incompatible backup file (version mismatch?).</p>"
              "<p><a href='/'>Back</a></p></div>";
        pg += FPSTR(HTML_FOOT);
        req->send(400, "text/html", pg);
      }
    },
    // onUpload — accumulate file chunks into _restore_buf
    [this](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) _restore_buf = "";
      if (_restore_buf.length() + len < 32768) {  // 32 KB safety cap
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
      req->redirect("/");
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
