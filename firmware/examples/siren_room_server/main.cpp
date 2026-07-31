#include <Arduino.h>
#include <Mesh.h>

#include "MyMesh.h"
#include "SettingsMenu.h"
#ifdef ENABLE_WIFI_MGMT
  #include "WebManager.h"
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
#ifdef ENABLE_WIFI_MGMT
  WebManager web_manager(the_mesh);
#endif

void halt() { while (1); }

static char command[MAX_POST_TEXT_LEN + 1];

void setup() {
  Serial.begin(115200);
  delay(1000);

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
  SPIFFS.begin(true);
  fs = &SPIFFS;
#else
  #error "Unsupported platform"
#endif

  sensors.begin();
  the_mesh.begin(fs);

#ifdef ENABLE_WIFI_MGMT
  web_manager.begin();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  command[0] = 0;
  board.onBootComplete();

  Serial.println("[SIREN] Phase 1 — multi-room server ready");
  Serial.println("Commands: menu | room list | room add | room del <idx>");
  Serial.println("          room set <idx> name <n> | pass <p> | guest <p>");
#ifdef ENABLE_WIFI_MGMT
  Serial.println("          wifi mode ap|sta | wifi ap ssid <n> | wifi ap pass <p>");
  Serial.println("          wifi ssid <n> | wifi pass <p> | wifi connect | wifi status");
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
        char reply[160];
#ifdef ENABLE_WIFI_MGMT
        if (memcmp(command, "wifi ", 5) == 0) {
          web_manager.handleWifiCommand(command + 5, reply);
        } else {
          the_mesh.handleCommand(0, command, reply);
        }
#else
        the_mesh.handleCommand(0, command, reply);
#endif
        if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
      }
      command[0] = 0;
    }
  }

  the_mesh.loop();
  sensors.loop();
#ifdef ENABLE_WIFI_MGMT
  web_manager.loop();
#endif
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
}
