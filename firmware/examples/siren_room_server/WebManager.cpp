#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include "WebManager.h"
#include <AsyncElegantOTA.h>

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

// Default AP SSID prefix — node name is appended at runtime
#define AP_SSID_PREFIX "SIREN-"

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------
WebManager::WebManager(MultiRoomMesh& mesh)
  : _server(80), _mesh(mesh), _started(false),
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
  Serial.printf("[WiFi] AP ready — IP: %s\n",
                WiFi.softAPIP().toString().c_str());
  if (!_started) {
    setupRoutes();
    _started = true;
    Serial.printf("[WiFi] Web UI: http://%s/\n",
                  WiFi.softAPIP().toString().c_str());
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
  page += "<table><tr><th>#</th><th>Name</th><th>Clients</th><th>Posts</th><th>Actions</th></tr>";
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!mesh.isRoomActive(i)) continue;
    page += "<tr><td>"; page += i;
    page += "</td><td>"; page += mesh.getRoomName(i);
    page += "</td><td>"; page += mesh.getRoomClientCount(i);
    page += "</td><td>"; page += mesh.getRoomPostCount(i);
    page += "</td><td>";
    if (i > 0) {
      page += "<form method='post' action='/api/room/del' style='display:inline'>"
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

  // OTA link
  page += "<div class='card'><a href='/update'>OTA Firmware Update</a></div>";

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
