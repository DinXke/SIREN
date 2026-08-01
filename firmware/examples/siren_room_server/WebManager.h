#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "MyMesh.h"
#include "UITask.h"

class MqttManager;  // forward declaration to avoid circular include

#define WIFI_CONFIG_PATH      "/wifi_sta.json"
#define WIFI_CONNECT_TIMEOUT_MS  15000

/**
 * WebManager — WiFi AP/STA + embedded web management UI.
 *
 * Default mode: AP (node creates its own hotspot, accessible at 192.168.4.1).
 * Optional: STA mode (connects to an existing WiFi network).
 *
 * Config persisted in SPIFFS (WIFI_CONFIG_PATH) as JSON:
 *   {"mode":"ap","ap_ssid":"SIREN-Node","ap_pass":"","sta_ssid":"...","sta_pass":"..."}
 *
 * CLI commands (handled via "wifi ..." prefix in main.cpp):
 *   wifi mode ap            — switch to AP mode and save
 *   wifi mode sta           — switch to STA mode and save
 *   wifi ap ssid <name>     — set AP SSID and save
 *   wifi ap pass <pass>     — set AP password (empty = open) and save
 *   wifi ssid <name>        — set STA SSID and save
 *   wifi pass <pass>        — set STA password and save
 *   wifi connect            — (re)connect STA with current credentials
 *   wifi status             — print current WiFi state to Serial
 */
class WebManager {
public:
  enum WifiMode { MODE_AP, MODE_STA };

private:
  AsyncWebServer   _server;
  DNSServer        _dns;
  MultiRoomMesh&   _mesh;
  UITask*          _ui_task;
  MqttManager*     _mqtt_mgr;
  bool             _started;
  bool             _dns_started;

  // Mode
  WifiMode         _mode;

  // AP credentials
  char             _ap_ssid[64];
  char             _ap_pass[64];

  // STA credentials + connection state
  char             _sta_ssid[64];
  char             _sta_pass[64];
  unsigned long    _connect_started;
  bool             _connecting;

  // Accumulation buffer for POST /api/restore upload body
  String           _restore_buf;

  void loadConfig();
  void saveConfig();
  void setupRoutes();
  void startAP();
  void stopCaptivePortal();
  void connectSTA();

  String buildStatusPage(const char* ip);
  String buildChatPage();
  String buildAclPage();
  String buildStatsPage();
  String buildStatsJson();

  String buildBackupJson();
  bool   applyRestore(const String& json);

  static String buildRoomsJson(MultiRoomMesh& mesh);

public:
  explicit WebManager(MultiRoomMesh& mesh);

  /** Wire up the UITask so the web UI can read/set screensaver config. */
  void setUITask(UITask* t) { _ui_task = t; }

  /** Wire up the MqttManager so the web UI can show MQTT status and config. */
  void setMqttManager(MqttManager* m) { _mqtt_mgr = m; }

  /** Call once after the_mesh.begin() and SPIFFS is mounted. */
  void begin();

  /** Call every loop iteration. */
  void loop();

  bool isConnected() const {
    return _mode == MODE_AP
      ? (WiFi.softAPgetStationNum() >= 0)   // AP always "up"
      : (WiFi.status() == WL_CONNECTED);
  }

  WifiMode getMode()     const { return _mode; }
  const char* getSSID()  const { return _mode == MODE_AP ? _ap_ssid : _sta_ssid; }

  /**
   * Handle "wifi ..." CLI commands.
   * Returns true if the command was consumed.
   */
  bool handleWifiCommand(const char* args, char* reply);
};

#endif // ESP32
