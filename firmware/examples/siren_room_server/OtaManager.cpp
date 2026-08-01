#ifdef ESP32
#ifdef ENABLE_WIFI_MGMT

#include "OtaManager.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <Arduino.h>

// Download chunk size (bytes) — small enough to fit comfortably in stack/heap
#define OTA_CHUNK_SIZE  4096

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

static String otaHtmlEsc(const char* s) {
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

/** Minimal JSON string extractor: finds "key":"value" in json, copies to out. */
static bool jsonGetStr(const char* json, const char* key,
                       char* out, size_t out_len) {
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  const char* e = strchr(p, '"');
  if (!e) return false;
  size_t n = (size_t)(e - p);
  if (n >= out_len) n = out_len - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// Build binary download URL for the current hardware target.
static const char* binUrl() {
  static char url[200];
  if (url[0] == 0) {
    snprintf(url, sizeof(url), OTA_BIN_URL_FMT,
             SIREN_HARDWARE_TARGET, SIREN_HARDWARE_TARGET);
  }
  return url;
}

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------

OtaManager::OtaManager()
  : _state(OtaState::IDLE), _progress(0), _task(nullptr)
{
  _avail_version[0] = 0;
  _avail_sha256[0]  = 0;
  _error_msg[0]     = 0;
}

void OtaManager::setError(const char* msg) {
  strncpy(_error_msg, msg, sizeof(_error_msg) - 1);
  _error_msg[sizeof(_error_msg) - 1] = 0;
  _state = OtaState::ERROR;
  Serial.printf("[OTA] Error: %s\n", _error_msg);
}

// ---------------------------------------------------------------------------
//  Manifest fetch
// ---------------------------------------------------------------------------

bool OtaManager::fetchManifest() {
  Serial.printf("[OTA] Checking manifest: %s\n", OTA_MANIFEST_URL);

  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle; security is SHA-256 of the binary itself

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, OTA_MANIFEST_URL)) {
    setError("http.begin failed (manifest)");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    char buf[64];
    snprintf(buf, sizeof(buf), "manifest HTTP %d", code);
    setError(buf);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  Serial.printf("[OTA] Manifest: %.200s\n", body.c_str());

  // Parse top-level "version" field
  char ver[32] = {};
  if (!jsonGetStr(body.c_str(), "version", ver, sizeof(ver))) {
    setError("manifest missing 'version'");
    return false;
  }

  // Locate the target sub-object, e.g. "v3":{...}
  char tgt_key[16];
  snprintf(tgt_key, sizeof(tgt_key), "\"%s\":", SIREN_HARDWARE_TARGET);
  const char* tgt = strstr(body.c_str(), tgt_key);
  if (!tgt) {
    setError("manifest: target key not found");
    return false;
  }

  // Extract sha256 from within the target sub-object
  char sha256[65] = {};
  if (!jsonGetStr(tgt, "sha256", sha256, sizeof(sha256))) {
    setError("manifest: sha256 missing for target");
    return false;
  }
  if (strlen(sha256) != 64) {
    setError("manifest: sha256 has wrong length (expected 64 hex chars)");
    return false;
  }

  strncpy(_avail_version, ver,    sizeof(_avail_version) - 1);
  strncpy(_avail_sha256,  sha256, sizeof(_avail_sha256)  - 1);
  _avail_version[sizeof(_avail_version) - 1] = 0;
  _avail_sha256 [sizeof(_avail_sha256)  - 1] = 0;

  Serial.printf("[OTA] Available: %s  SHA256: %.16s...\n",
                _avail_version, _avail_sha256);
  return true;
}

// ---------------------------------------------------------------------------
//  OTA flash
// ---------------------------------------------------------------------------

bool OtaManager::flashImage() {
  Serial.printf("[OTA] Downloading: %s\n", binUrl());

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(client, binUrl())) {
    setError("http.begin failed (binary)");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    char buf[64];
    snprintf(buf, sizeof(buf), "binary HTTP %d", code);
    setError(buf);
    http.end();
    return false;
  }

  int content_length = http.getSize();
  Serial.printf("[OTA] Content-Length: %d bytes\n", content_length);

  // Locate inactive OTA partition
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) {
    setError("no inactive OTA partition found");
    http.end();
    return false;
  }
  Serial.printf("[OTA] Target partition: %s @ 0x%08x  size=%u\n",
                part->label, (unsigned)part->address, (unsigned)part->size);

  if (content_length > 0 && (size_t)content_length > part->size) {
    setError("image too large for OTA partition");
    http.end();
    return false;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    char buf[80];
    snprintf(buf, sizeof(buf), "esp_ota_begin: 0x%x", (unsigned)err);
    setError(buf);
    http.end();
    return false;
  }

  // Stream download, hash on-the-fly with mbedTLS SHA-256
  WiFiClient* stream = http.getStreamPtr();

  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, 0 /* 0=SHA-256 */);

  static uint8_t chunk[OTA_CHUNK_SIZE];  // static: keeps off task stack
  int bytes_written = 0;
  bool ok = true;

  while (http.connected() &&
         (content_length < 0 || bytes_written < content_length)) {
    size_t avail = stream->available();
    if (!avail) { vTaskDelay(1); continue; }

    size_t to_read = avail < OTA_CHUNK_SIZE ? avail : OTA_CHUNK_SIZE;
    size_t n       = stream->readBytes(chunk, to_read);
    if (n == 0) break;

    mbedtls_sha256_update(&sha_ctx, chunk, n);

    err = esp_ota_write(ota_handle, chunk, n);
    if (err != ESP_OK) {
      char buf[80];
      snprintf(buf, sizeof(buf), "esp_ota_write: 0x%x", (unsigned)err);
      setError(buf);
      ok = false;
      break;
    }

    bytes_written += (int)n;
    if (content_length > 0)
      _progress = (bytes_written * 100) / content_length;

    vTaskDelay(0);  // yield to other tasks each chunk
  }

  http.end();

  if (!ok) {
    esp_ota_abort(ota_handle);
    mbedtls_sha256_free(&sha_ctx);
    return false;
  }

  if (content_length > 0 && bytes_written != content_length) {
    char buf[80];
    snprintf(buf, sizeof(buf), "incomplete download: %d/%d bytes",
             bytes_written, content_length);
    setError(buf);
    esp_ota_abort(ota_handle);
    mbedtls_sha256_free(&sha_ctx);
    return false;
  }

  // Finalise SHA-256 and convert to hex
  uint8_t sha_raw[32];
  mbedtls_sha256_finish(&sha_ctx, sha_raw);
  mbedtls_sha256_free(&sha_ctx);

  char computed[65] = {};
  for (int i = 0; i < 32; i++)
    snprintf(computed + i * 2, 3, "%02x", (unsigned)sha_raw[i]);

  Serial.printf("[OTA] SHA-256 computed: %s\n", computed);
  Serial.printf("[OTA] SHA-256 expected: %s\n", _avail_sha256);

  // INTEGRITY GATE — reject before any partition change
  if (strcasecmp(computed, _avail_sha256) != 0) {
    setError("SHA-256 mismatch — image rejected");
    esp_ota_abort(ota_handle);
    return false;
  }

  // SHA-256 OK — commit and set boot partition
  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    char buf[80];
    snprintf(buf, sizeof(buf), "esp_ota_end: 0x%x", (unsigned)err);
    setError(buf);
    return false;
  }

  err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    char buf[80];
    snprintf(buf, sizeof(buf), "esp_ota_set_boot_partition: 0x%x", (unsigned)err);
    setError(buf);
    return false;
  }

  _progress = 100;
  Serial.printf("[OTA] Flash complete (%d bytes). Rebooting to %s...\n",
                bytes_written, _avail_version);
  return true;
}

// ---------------------------------------------------------------------------
//  FreeRTOS task
// ---------------------------------------------------------------------------

void OtaManager::taskWrapper(void* self) {
  ((OtaManager*)self)->runTask();
}

void OtaManager::runTask() {
  if (_state == OtaState::CHECKING) {
    if (fetchManifest()) {
      if (strcmp(_avail_version, FIRMWARE_VERSION) == 0) {
        _state = OtaState::UP_TO_DATE;
        Serial.printf("[OTA] Firmware up-to-date (%s)\n", FIRMWARE_VERSION);
      } else {
        _state = OtaState::UPDATE_AVAILABLE;
        Serial.printf("[OTA] Update available: %s -> %s\n",
                      FIRMWARE_VERSION, _avail_version);
      }
    }
    // on error: setError() already set _state = ERROR
  } else if (_state == OtaState::DOWNLOADING) {
    if (flashImage()) {
      _state = OtaState::DONE;
      delay(1500);         // brief pause so status page can show "Done"
      ESP.restart();
    }
  }

  _task = nullptr;
  vTaskDelete(nullptr);
}

void OtaManager::startCheck() {
  if (_state == OtaState::CHECKING || _state == OtaState::DOWNLOADING) return;
  _state            = OtaState::CHECKING;
  _progress         = 0;
  _error_msg[0]     = 0;
  _avail_version[0] = 0;
  _avail_sha256[0]  = 0;
  xTaskCreate(taskWrapper, "ota_check", OTA_TASK_STACK, this, 1, &_task);
}

bool OtaManager::startUpdate() {
  if (_state != OtaState::UPDATE_AVAILABLE) return false;
  _state        = OtaState::DOWNLOADING;
  _progress     = 0;
  _error_msg[0] = 0;
  xTaskCreate(taskWrapper, "ota_flash", OTA_TASK_STACK, this, 1, &_task);
  return true;
}

// ---------------------------------------------------------------------------
//  Web section
// ---------------------------------------------------------------------------

String OtaManager::buildWebSection() const {
  String s = "<div class='card'><h2>Firmware Update</h2>";
  s += "<p>Huidige versie: <strong>" FIRMWARE_VERSION "</strong>"
       " &nbsp; Doel: <strong>" SIREN_HARDWARE_TARGET "</strong></p>";

  switch (_state) {
    case OtaState::IDLE:
      s += "<form method='post' action='/api/ota/check'>"
           "<button type='submit'>Controleer op update</button></form>";
      break;

    case OtaState::CHECKING:
      s += "<p>&#x23F3; Manifest ophalen...</p>"
           "<script>setTimeout(function(){location.reload();},2000);</script>";
      break;

    case OtaState::UP_TO_DATE:
      s += "<p style='color:green'>&#x2705; Firmware is up-to-date.</p>"
           "<form method='post' action='/api/ota/check'>"
           "<button type='submit'>Opnieuw controleren</button></form>";
      break;

    case OtaState::UPDATE_AVAILABLE: {
      String v = otaHtmlEsc(_avail_version);
      s += "<p>Beschikbare versie: <strong>";
      s += v;
      s += "</strong></p>"
           "<form method='post' action='/api/ota/update' "
           "onsubmit=\"return confirm('Firmware bijwerken naar ";
      s += v;
      s += "?\\nDe node herstart automatisch na het flashen.')\">"
           "<button type='submit' "
           "style='background:#c0392b;color:#fff;padding:8px 20px'>"
           "&#x26A1; Nu bijwerken</button></form>";
      break;
    }

    case OtaState::DOWNLOADING: {
      int p = _progress;
      s += "<p>&#x23EC; Downloaden + flashen: ";
      s += p;
      s += "%</p><progress value='";
      s += p;
      s += "' max='100' style='width:100%;height:20px'></progress>"
           "<script>setTimeout(function(){location.reload();},1500);</script>";
      break;
    }

    case OtaState::DONE:
      s += "<p style='color:green'>&#x2705; Update geslaagd! Herstart...</p>";
      break;

    case OtaState::ERROR:
      s += "<p style='color:red'>&#x274C; Fout: ";
      s += otaHtmlEsc(_error_msg);
      s += "</p><form method='post' action='/api/ota/check'>"
           "<button type='submit'>Opnieuw proberen</button></form>";
      break;
  }

  s += "<p style='font-size:0.85em;color:#aaa'>"
       "OTA overschrijft alleen de app-partitie. "
       "Kanalen, sleutels en instellingen (SPIFFS/NVS) blijven bewaard. "
       "CLI: <code>ota check</code> | <code>ota update</code> | <code>ota status</code></p>"
       "</div>";
  return s;
}

// ---------------------------------------------------------------------------
//  CLI handler
// ---------------------------------------------------------------------------

bool OtaManager::handleCommand(const char* args, char* reply) {
  while (*args == ' ') args++;

  if (strcmp(args, "check") == 0) {
    if (_state == OtaState::CHECKING || _state == OtaState::DOWNLOADING) {
      strcpy(reply, "OTA bezig — wacht tot klaar");
    } else {
      startCheck();
      strcpy(reply, "OTA check gestart (zie web UI of serieel voor resultaat)");
    }
    return true;
  }

  if (strcmp(args, "update") == 0) {
    if (_state == OtaState::UPDATE_AVAILABLE) {
      startUpdate();
      strcpy(reply, "OTA update gestart — node herstart na flashen");
    } else if (_state == OtaState::CHECKING || _state == OtaState::DOWNLOADING) {
      strcpy(reply, "OTA bezig — wacht tot klaar");
    } else if (_state == OtaState::UP_TO_DATE) {
      strcpy(reply, "Firmware is al up-to-date");
    } else {
      strcpy(reply, "Voer eerst 'ota check' uit");
    }
    return true;
  }

  if (strcmp(args, "status") == 0) {
    switch (_state) {
      case OtaState::IDLE:
        snprintf(reply, 160, "OTA: idle. Huidig: %s. Voer 'ota check' uit.",
                 FIRMWARE_VERSION);
        break;
      case OtaState::CHECKING:
        strcpy(reply, "OTA: manifest ophalen...");
        break;
      case OtaState::UP_TO_DATE:
        snprintf(reply, 160, "OTA: up-to-date (%s)", FIRMWARE_VERSION);
        break;
      case OtaState::UPDATE_AVAILABLE:
        snprintf(reply, 160, "OTA: update beschikbaar: %s -> %s. Typ 'ota update'.",
                 FIRMWARE_VERSION, _avail_version);
        break;
      case OtaState::DOWNLOADING:
        snprintf(reply, 160, "OTA: downloaden %d%%", _progress);
        break;
      case OtaState::DONE:
        strcpy(reply, "OTA: klaar, herstart...");
        break;
      case OtaState::ERROR:
        snprintf(reply, 160, "OTA fout: %s", _error_msg);
        break;
    }
    return true;
  }

  snprintf(reply, 160, "Gebruik: ota check | ota update | ota status");
  return true;
}

#endif // ENABLE_WIFI_MGMT
#endif // ESP32
