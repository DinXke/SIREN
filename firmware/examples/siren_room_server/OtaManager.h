#pragma once
#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Manifest URL — public GitHub raw, branch multiroom
#define OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/DinXke/SIREN/multiroom/firmware/dist/version.json"

// Binary download URL — derived from SIREN_HARDWARE_TARGET at runtime
// e.g. firmware/dist/heltec_v3/SIREN_v3_room_server.bin
#define OTA_BIN_URL_FMT \
  "https://raw.githubusercontent.com/DinXke/SIREN/multiroom/firmware/dist/heltec_%s/SIREN_%s_room_server.bin"

// Hardware target string baked in per-target platformio.ini
#ifndef SIREN_HARDWARE_TARGET
  #define SIREN_HARDWARE_TARGET "v3"
#endif

// Current version baked in at build time
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "unknown"
#endif

// FreeRTOS task stack for HTTPS download (TLS needs ~40 KB heap, not stack)
#define OTA_TASK_STACK  8192

enum class OtaState : uint8_t {
  IDLE             = 0,  // no action taken yet
  CHECKING         = 1,  // fetching version manifest
  UP_TO_DATE       = 2,  // manifest fetched, versions match
  UPDATE_AVAILABLE = 3,  // manifest fetched, newer version exists
  DOWNLOADING      = 4,  // downloading + flashing image
  DONE             = 5,  // image flashed OK, rebooting
  ERROR            = 6,  // something went wrong (_error_msg set)
};

/**
 * OtaManager — GitHub self-update over WiFi STA.
 *
 * Flow:
 *   1. startCheck()     — fetches OTA_MANIFEST_URL, parses version + sha256
 *   2. startUpdate()    — downloads binary, verifies SHA-256, flashes to
 *                         inactive OTA partition, marks boot partition, reboots.
 *
 * Security:
 *   - WiFiClientSecure with setInsecure() (no CA bundle needed for raw.githubusercontent.com)
 *   - Mandatory SHA-256 verification of the downloaded image against the manifest
 *     before esp_ota_end() / esp_ota_set_boot_partition() are called.
 *   - A corrupt or wrong image is rejected BEFORE the boot partition is changed.
 *   - Rollback: main.cpp calls esp_ota_mark_app_valid_cancel_rollback() in setup()
 *     so a bad image that somehow boots will auto-rollback after watchdog reset.
 *
 * Settings (SPIFFS/NVS): OTA flashes the app partition only — SPIFFS and NVS
 * are never touched.  All channels, keys and prefs survive.
 *
 * Partition check: partitions_siren.csv has two 3.1 MB OTA slots; current
 * image is ~1.3 MB.  No partition-table change needed.
 */
class OtaManager {
public:
  OtaManager();

  /** Asynchronously fetch the version manifest.  No-op if busy. */
  void startCheck();

  /** Asynchronously download + flash the update.
   *  Returns false if state != UPDATE_AVAILABLE. */
  bool startUpdate();

  OtaState    getState()            const { return _state; }
  int         getProgress()         const { return _progress; }   // 0-100
  const char* getAvailableVersion() const { return _avail_version; }
  const char* getErrorMsg()         const { return _error_msg; }

  /** Build the firmware-update card HTML for the management status page. */
  String buildWebSection() const;

  /** Handle "ota ..." serial CLI commands.  Returns true if consumed. */
  bool handleCommand(const char* args, char* reply);

private:
  volatile OtaState _state;
  volatile int      _progress;           // 0-100 while DOWNLOADING
  char              _avail_version[32];  // version from manifest
  char              _avail_sha256[65];   // 64-hex SHA-256 from manifest + NUL
  char              _error_msg[128];
  TaskHandle_t      _task;

  void setError(const char* msg);

  static void taskWrapper(void* self);
  void        runTask();

  bool fetchManifest();   // runs synchronously inside task
  bool flashImage();      // runs synchronously inside task
};

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
