#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "MyMesh.h"

#define WIFI_CONFIG_PATH  "/wifi_sta.json"
#define WIFI_CONNECT_TIMEOUT_MS  15000

/**
 * WebManager — Phase 9 WiFi STA + embedded web management UI.
 *
 * Reads WiFi credentials from SPIFFS (/wifi_sta.json), connects as STA,
 * then serves a management web UI on port 80.  OTA updates available at /update.
 *
 * CLI commands (handled by MultiRoomMesh::handleCommand via "wifi ..." prefix):
 *   wifi ssid <name>   — set SSID and save to SPIFFS
 *   wifi pass <pass>   — set password and save to SPIFFS
 *   wifi connect       — (re)connect with current credentials
 *   wifi status        — print current WiFi state to Serial
 */
class WebManager {
  AsyncWebServer   _server;
  MultiRoomMesh&   _mesh;
  bool             _started;
  unsigned long    _connect_started;
  bool             _connecting;
  char             _ssid[64];
  char             _pass[64];

  void loadConfig();
  void saveConfig();
  void setupRoutes();
  void connect();

  // HTML helpers — all returned strings live in PROGMEM / static storage
  static String buildStatusPage(MultiRoomMesh& mesh, const char* ip);
  static String buildRoomsJson(MultiRoomMesh& mesh);

public:
  explicit WebManager(MultiRoomMesh& mesh);

  /** Call once after the_mesh.begin() and SPIFFS is mounted. */
  void begin();

  /** Call every loop iteration to check connection state. */
  void loop();

  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  const char* getSSID() const { return _ssid; }

  /**
   * Handle "wifi ..." CLI commands.
   * Returns true if the command was consumed, false otherwise.
   */
  bool handleWifiCommand(const char* args, char* reply);
};

#endif // ESP32
