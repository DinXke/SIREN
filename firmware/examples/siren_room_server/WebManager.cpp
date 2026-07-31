#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include "WebManager.h"
#include <AsyncElegantOTA.h>

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------
WebManager::WebManager(MultiRoomMesh& mesh)
  : _server(80), _mesh(mesh), _started(false),
    _connect_started(0), _connecting(false)
{
  _ssid[0] = 0;
  _pass[0] = 0;
}

// ---------------------------------------------------------------------------
//  Config persistence (tiny hand-rolled JSON to avoid ArduinoJson dep)
// ---------------------------------------------------------------------------
void WebManager::loadConfig() {
  File f = SPIFFS.open(WIFI_CONFIG_PATH, "r");
  if (!f) return;

  // Format: {"ssid":"...","pass":"..."}
  // Simple extraction without a JSON library.
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

  extractField("ssid", _ssid, sizeof(_ssid));
  extractField("pass", _pass, sizeof(_pass));
}

void WebManager::saveConfig() {
  File f = SPIFFS.open(WIFI_CONFIG_PATH, "w");
  if (!f) return;
  f.printf("{\"ssid\":\"%s\",\"pass\":\"%s\"}", _ssid, _pass);
  f.close();
}

// ---------------------------------------------------------------------------
//  Connect
// ---------------------------------------------------------------------------
void WebManager::connect() {
  if (_ssid[0] == 0) return;

  Serial.printf("[WiFi] Connecting to '%s'...\n", _ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(_ssid, _pass[0] ? _pass : nullptr);
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
  "input{background:#2a2a4a;border:1px solid #555;color:#e0e0e0;padding:4px;margin:2px}"
  "button{background:#00d4ff;border:none;color:#000;padding:5px 12px;cursor:pointer;margin:2px}"
  ".card{background:#16213e;border:1px solid #333;padding:10px;margin-bottom:10px;border-radius:4px}"
  ".ok{color:#00ff88}.err{color:#ff4444}.warn{color:#ffcc00}"
  "a{color:#00d4ff}"
  "</style></head><body>";

static const char HTML_FOOT[] PROGMEM = "</body></html>";

String WebManager::buildStatusPage(MultiRoomMesh& mesh, const char* ip) {
  String page = FPSTR(HTML_HEAD);

  page += "<div class='card'><h2>SIREN Room Server</h2>";
  page += "<table>";
  page += "<tr><th>Node</th><td>"; page += mesh.getNodeName(); page += "</td></tr>";
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
  // Add room form
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
  page += "<div class='card'><h2>WiFi</h2>"
          "<form method='post' action='/api/wifi'>"
          "SSID: <input name='ssid' size='24'> "
          "Password: <input name='pass' type='password' size='24'> "
          "<button type='submit'>Save &amp; Reconnect</button></form>"
          "<p>Current SSID: <b>"; page += WiFi.SSID();
  page += "</b> &nbsp; IP: <b>"; page += ip;
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
    String ip = WiFi.localIP().toString();
    req->send(200, "text/html", buildStatusPage(_mesh, ip.c_str()));
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
      char cmd[160];
      char reply[160];

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

  // API: update WiFi credentials
  _server.on("/api/wifi", HTTP_POST,
    [this, user, pass](AsyncWebServerRequest* req) {
      if (!req->authenticate(user, pass)) return req->requestAuthentication();
      if (req->hasParam("ssid", true)) {
        strncpy(_ssid, req->getParam("ssid", true)->value().c_str(), sizeof(_ssid) - 1);
        _ssid[sizeof(_ssid) - 1] = 0;
      }
      if (req->hasParam("pass", true)) {
        strncpy(_pass, req->getParam("pass", true)->value().c_str(), sizeof(_pass) - 1);
        _pass[sizeof(_pass) - 1] = 0;
      }
      saveConfig();
      req->redirect("/");
      // Reconnect asynchronously after reply is sent
      WiFi.disconnect(false);
      _connecting = false;
      connect();
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
  if (_ssid[0]) connect();
}

void WebManager::loop() {
  AsyncElegantOTA.loop();

  if (_connecting) {
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
      Serial.printf("[WiFi] Connect timeout — status %d. Use 'wifi connect' to retry.\n", st);
    }
  }
}

bool WebManager::handleWifiCommand(const char* args, char* reply) {
  while (*args == ' ') args++;

  if (memcmp(args, "ssid ", 5) == 0) {
    strncpy(_ssid, args + 5, sizeof(_ssid) - 1);
    _ssid[sizeof(_ssid) - 1] = 0;
    saveConfig();
    sprintf(reply, "OK - SSID set to '%s' (use 'wifi connect' to connect)", _ssid);
    return true;
  }

  if (memcmp(args, "pass ", 5) == 0) {
    strncpy(_pass, args + 5, sizeof(_pass) - 1);
    _pass[sizeof(_pass) - 1] = 0;
    saveConfig();
    strcpy(reply, "OK - WiFi password saved");
    return true;
  }

  if (strcmp(args, "connect") == 0) {
    if (_ssid[0] == 0) { strcpy(reply, "Err - no SSID set (use 'wifi ssid <name>')"); return true; }
    WiFi.disconnect(false);
    _connecting = false;
    connect();
    sprintf(reply, "OK - connecting to '%s'...", _ssid);
    return true;
  }

  if (strcmp(args, "status") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      sprintf(reply, "Connected: SSID='%s' IP=%s", WiFi.SSID().c_str(),
              WiFi.localIP().toString().c_str());
    } else if (_connecting) {
      sprintf(reply, "Connecting to '%s'...", _ssid);
    } else if (_ssid[0]) {
      sprintf(reply, "Disconnected (SSID='%s')", _ssid);
    } else {
      strcpy(reply, "No WiFi configured");
    }
    return true;
  }

  strcpy(reply, "Err - usage: wifi ssid <name> | wifi pass <pw> | wifi connect | wifi status");
  return true;
}

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
