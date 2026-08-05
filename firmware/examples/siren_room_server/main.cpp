#include <Arduino.h>
#include <Mesh.h>
#ifdef ESP32
  #include <esp_ota_ops.h>
  #include <esp_system.h>
  #if defined(SIREN_SD_CARD)
    #include <SD.h>
  #endif
#endif

#include "MyMesh.h"
#include "SettingsMenu.h"
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
  #include "WebManager.h"
  #include "MqttManager.h"
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MultiRoomMesh the_mesh(board, radio_driver,
                       *new ArduinoMillis(), fast_rng,
                       rtc_clock, tables);
SettingsMenu settings_menu(the_mesh);
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
  WebManager  web_manager(the_mesh);
  MqttManager mqtt_manager(the_mesh);
#endif

void halt() { while (1); }

static char command[MAX_POST_TEXT_LEN + 1];

void setup() {
  Serial.begin(115200);
  delay(1000);

#ifdef ESP32
  // Log reset reason so crash cause is visible in serial output (JES-864)
  {
    static const char* const rst_names[] = {
      "UNKNOWN", "POWERON", "EXT/RST_PIN", "SOFTWARE",
      "PANIC/ABORT", "INT_WDT", "TASK_WDT", "WDT",
      "DEEPSLEEP", "BROWNOUT", "SDIO"
    };
    esp_reset_reason_t rr = esp_reset_reason();
    int rr_idx = ((int)rr >= 0 && (int)rr <= 10) ? (int)rr : 0;
    Serial.printf("[BOOT] Reset reason: %s (%d)\n", rst_names[rr_idx], (int)rr);
    Serial.printf("[BOOT] Free heap: %u bytes\n", esp_get_free_heap_size());
  }
#endif

  board.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("SIREN boot...");
    display.endFrame();
  }
#endif

  if (!radio_init()) halt();

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
#elif defined(ESP32)
  // SD offload (JES-851): when an SD/FAT filesystem is compiled in, prefer it
  // for room persistence AND the large post archive, falling back to SPIFFS
  // when no card is present or mounting fails.
  bool sd_ok = false;
  #if defined(SIREN_SD_CARD)
    #if defined(SIREN_SD_SCK) && defined(SIREN_SD_MISO) && defined(SIREN_SD_MOSI)
      // SD card on a dedicated SPI bus (e.g. VSPI pins on the T-LoRa V2.1),
      // separate from the LoRa radio SPI.
      SPI.begin(SIREN_SD_SCK, SIREN_SD_MISO, SIREN_SD_MOSI, SIREN_SD_CS);
    #endif
    sd_ok = SD.begin(SIREN_SD_CS);
    if (sd_ok) {
      Serial.println("[SIREN] SD card mounted — using it as primary filesystem");
      fs = &SD;
    }
  #endif
  if (!sd_ok) {
    SPIFFS.begin(true);
    fs = &SPIFFS;
  }
#else
  #error "Unsupported platform"
#endif

  sensors.begin();
  the_mesh.begin(fs);

#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
  web_manager.begin();

  // MqttManager: register post-publish hook into the mesh, then start
  the_mesh.setPostPublishCallback(
    [](int room_idx, uint32_t ts, const uint8_t* auth, const char* txt, void* ctx) {
      ((MqttManager*)ctx)->onPostAdded(room_idx, ts, auth, txt);
    }, &mqtt_manager);
  mqtt_manager.begin();
  web_manager.setMqttManager(&mqtt_manager);
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
  #if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
    web_manager.setUITask(&ui_task);
  #endif
#endif

#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  command[0] = 0;
  board.onBootComplete();

#ifdef ESP32
  // Mark this image as valid so the bootloader cancels any pending rollback
  // (rollback is triggered if the watchdog fires before this call is reached).
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  Serial.println("[SIREN] Phase 3 — multi-room server with CLI + NVS persistence");
  Serial.println("Commands: menu");
  Serial.println("  room list | add | del <idx> | set <idx> name|pass|guest <v>");
  Serial.println("  room clients <idx> | setperm <idx> <hex> <perms> | status <idx>");
  Serial.println("  room stealth <idx> on|off | qr <idx>");
  Serial.println("  rooms | msgs <idx> [n] | nicks <idx> | say <idx> <text>");
  Serial.println("  peer list | add <hex64> <name> | del <idx> | status | sync");
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
  Serial.println("  wifi mode ap|sta | wifi ap ssid <n> | wifi ap pass <p>");
  Serial.println("  wifi ssid <n> | wifi pass <p> | wifi connect | wifi status");
  Serial.println("  mqtt status | enable | disable | set host|port|tls|user|pass|net_id ...");
  Serial.println("  ota check | ota update | ota status");
#endif
  Serial.println("Type 'menu' + Enter to open the interactive settings menu.");
}

void loop() {
  if (settings_menu.isActive()) {
    /* Menu is open: feed every incoming byte to the menu state machine */
    while (Serial.available()) {
      settings_menu.feed((char)Serial.read());
    }
  } else {
    /* Normal serial CLI: accumulate line, dispatch on CR */
    int len = strlen(command);
    while (Serial.available() && len < (int)sizeof(command) - 1) {
      char c = Serial.read();
      if (c != '\n') { command[len++] = c; command[len] = 0; }
      Serial.print(c);
    }
    if (len == (int)sizeof(command) - 1) {
      command[sizeof(command) - 1] = '\r';
    }

    if (len > 0 && command[len - 1] == '\r') {
      command[len - 1] = 0;
      if (strcmp(command, "menu") == 0) {
        settings_menu.enter();
      } else {
        char reply[160] = {};
        bool handled = false;
#ifdef DISPLAY_CLASS
        if (!handled && memcmp(command, "screensaver", 11) == 0 &&
            (command[11] == 0 || command[11] == ' ')) {
          ui_task.handleCommand(command, reply);
          handled = true;
        }
#endif
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
        if (!handled && memcmp(command, "wifi ", 5) == 0) {
          web_manager.handleWifiCommand(command + 5, reply);
          handled = true;
        }
        if (!handled && memcmp(command, "mqtt", 4) == 0 &&
            (command[4] == 0 || command[4] == ' ')) {
          const char* mqtt_args = (command[4] == ' ') ? command + 5 : "status";
          mqtt_manager.handleMqttCommand(mqtt_args, reply);
          handled = true;
        }
        if (!handled && memcmp(command, "ota", 3) == 0 &&
            (command[3] == 0 || command[3] == ' ')) {
          const char* ota_args = (command[3] == ' ') ? command + 4 : "status";
          web_manager.handleOtaCommand(ota_args, reply);
          handled = true;
        }
#endif
        if (!handled) {
          the_mesh.handleCommand(0, command, reply);
        }
        if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
      }
      command[0] = 0;
    }
  }

  the_mesh.loop();
  sensors.loop();
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
  web_manager.loop();
  mqtt_manager.loop();
#endif
#ifdef DISPLAY_CLASS
  {
    UiStats ui_stats;
    ui_stats.room_count    = (uint8_t)the_mesh.getNumActiveRooms();
    ui_stats.total_posts   = the_mesh.getTotalPosts();
    ui_stats.uptime_ms     = (uint32_t)the_mesh.getUptimeMillis();
    ui_stats.wifi_mode     = 0;
    ui_stats.wifi_ip[0]    = 0;
    ui_stats.contact_count = the_mesh.getTotalContacts();
#if defined(ENABLE_WIFI_MGMT) && (ENABLE_WIFI_MGMT)
    ui_stats.wifi_mode = (uint8_t)(web_manager.getMode() + 1);  // MODE_AP=0->1, MODE_STA=1->2
    {
      String ip = (web_manager.getMode() == WebManager::MODE_AP)
                  ? WiFi.softAPIP().toString()
                  : WiFi.localIP().toString();
      strncpy(ui_stats.wifi_ip, ip.c_str(), sizeof(ui_stats.wifi_ip) - 1);
      ui_stats.wifi_ip[sizeof(ui_stats.wifi_ip) - 1] = 0;
    }
#endif
    ui_task.setStats(ui_stats);
  }
  ui_task.loop();
#endif
  rtc_clock.tick();
}
