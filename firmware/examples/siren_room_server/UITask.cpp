#include "UITask.h"
#include <Arduino.h>
#include <helpers/CommonCLI.h>

#ifdef ESP32
  #include <SPIFFS.h>
#endif

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#define AUTO_OFF_MILLIS      20000  // 20 s inactivity -> screensaver (or off)
#define BOOT_SCREEN_MILLIS    5000  // 5 s animated boot screen
#define BOOT_ANIM_FRAMES        20  // logo slides in over first 20 frames (1 s @ 50 ms/frame)
#define SS_PAGE_MILLIS         3000 // screensaver page dwell time (3 s)
#define KEEP_ON_DIM_MILLIS    60000 // if ss=off, keep-on=off: turn off after 60 s
#define SS_PAGES                 6  // number of screensaver stat pages

// Burn-in drift LUT — shifts content by a few pixels each page to protect OLED.
// Keep offsets small so all content stays fully on screen (128x64 with sz-1 text).
static const int8_t SS_DRIFT_X[SS_PAGES] = {  0,  4, -4,  2, -2,  6 };
static const int8_t SS_DRIFT_Y[SS_PAGES] = {  0,  2, -2,  4, -4,  0 };

// SPIFFS path for screensaver settings
#define SS_CFG_PATH "/ss_cfg"

// 'meshcore', 128x13px
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe,
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe,
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc,
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00,
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00,
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8,
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8,
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0,
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00,
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00,
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8,
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8,
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8,
};

/* ------------------------------------------------------------------ */
/*  SPIFFS persistence                                                  */
/* ------------------------------------------------------------------ */

void UITask::loadSsConfig() {
#ifdef ESP32
  File f = SPIFFS.open(SS_CFG_PATH, "r");
  if (!f) return;
  String raw = f.readString();
  f.close();

  auto extractBool = [&](const char* key) -> int {
    String k = String("\"") + key + "\":";
    int start = raw.indexOf(k);
    if (start < 0) return -1;
    start += k.length();
    while (start < (int)raw.length() && raw[start] == ' ') start++;
    return (raw[start] == '1') ? 1 : 0;
  };

  int ss   = extractBool("ss");
  int keep = extractBool("keep");
  if (ss   >= 0) _ss_enabled = (ss   == 1);
  if (keep >= 0) _ss_keep_on = (keep == 1);

  // Load page interval (integer field)
  String ki = String("\"interval\":");
  int istart = raw.indexOf(ki);
  if (istart >= 0) {
    istart += ki.length();
    while (istart < (int)raw.length() && raw[istart] == ' ') istart++;
    int val = raw.substring(istart).toInt();
    if (val >= 1 && val <= 60) _ss_page_sec = (uint8_t)val;
  }
#endif
}

void UITask::saveSsConfig() {
#ifdef ESP32
  File f = SPIFFS.open(SS_CFG_PATH, "w");
  if (!f) return;
  f.printf("{\"ss\":%d,\"keep\":%d,\"interval\":%d}",
           _ss_enabled ? 1 : 0, _ss_keep_on ? 1 : 0, (int)_ss_page_sec);
  f.close();
#endif
}

/* ------------------------------------------------------------------ */
/*  Public setters (persist immediately)                               */
/* ------------------------------------------------------------------ */

void UITask::setSsEnabled(bool v) {
  _ss_enabled = v;
  saveSsConfig();
  if (!v && _in_screensaver) {
    // Exit screensaver back to home when disabled
    _in_screensaver = false;
    if (_display->isOn()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
  }
}

void UITask::setSsKeepOn(bool v) {
  _ss_keep_on = v;
  saveSsConfig();
}

void UITask::setSsPageSec(uint8_t sec) {
  if (sec < 1)  sec = 1;
  if (sec > 60) sec = 60;
  _ss_page_sec = sec;
  saveSsConfig();
}

/* ------------------------------------------------------------------ */
/*  begin()                                                             */
/* ------------------------------------------------------------------ */

void UITask::begin(NodePrefs* node_prefs, const char* build_date,
                   const char* firmware_version) {
  _prevBtnState = HIGH;
  _node_prefs   = node_prefs;
  _auto_off     = millis() + AUTO_OFF_MILLIS;
  _display->turnOn();

  // Strip commit hash from version string ("v1.2.3-abcdef" -> "v1.2.3")
  char* version = strdup(firmware_version);
  char* dash    = strchr(version, '-');
  if (dash) *dash = 0;
  snprintf(_version_info, sizeof(_version_info), "%s (%s)", version, build_date);
  free(version);

  // Load screensaver settings (SPIFFS must be mounted before begin() is called)
  loadSsConfig();
}

/* ------------------------------------------------------------------ */
/*  renderBootScreen — procedural animation, no delay()                */
/* ------------------------------------------------------------------ */

void UITask::renderBootScreen() {
  unsigned long t = millis() / 50;  // frame counter: 0..99 over 5 s

  // Logo slide-in from above: arrives at y=3 after BOOT_ANIM_FRAMES frames
  int logo_y;
  if (t >= (unsigned long)BOOT_ANIM_FRAMES) {
    logo_y = 3;
  } else {
    // Interpolate y: from -13 (fully off-screen top) to 3
    logo_y = -13 + (int)(t * 16 / BOOT_ANIM_FRAMES);
  }

  // Only draw logo once it's at least partially visible
  if (logo_y > -13) {
    _display->setColor(DisplayDriver::BLUE);
    _display->drawXbm(0, logo_y, meshcore_logo, 128, 13);
  }

  // Text items appear after logo settles
  if (t >= (unsigned long)(BOOT_ANIM_FRAMES + 5)) {
    _display->setColor(DisplayDriver::LIGHT);
    _display->setTextSize(1);

    // Website
    const char* website = "https://meshcore.io";
    _display->drawTextCentered(_display->width() / 2, 22, website);

    // Version info
    _display->drawTextCentered(_display->width() / 2, 33, _version_info);

    // Branding line (SIREN by DinX)
    _display->drawTextCentered(_display->width() / 2, 42, "SIREN (by DinX)");
  }

  // Progress bar along the bottom (fills left to right over full boot duration)
  {
    int bar_w = (int)(128UL * millis() / BOOT_SCREEN_MILLIS);
    if (bar_w > 128) bar_w = 128;
    if (bar_w > 0) {
      _display->setColor(DisplayDriver::BLUE);
      _display->fillRect(0, 61, bar_w, 3);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  renderHomeScreen — static node info                                */
/* ------------------------------------------------------------------ */

void UITask::renderHomeScreen() {
  char tmp[80];

  // Node name (green, size 1)
  _display->setCursor(0, 0);
  _display->setTextSize(1);
  _display->setColor(DisplayDriver::GREEN);
  _display->print(_node_prefs->node_name);

  // FREQ / SF
  _display->setCursor(0, 20);
  _display->setColor(DisplayDriver::YELLOW);
  snprintf(tmp, sizeof(tmp), "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
  _display->print(tmp);

  // BW / CR
  _display->setCursor(0, 30);
  snprintf(tmp, sizeof(tmp), "BW: %05.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
  _display->print(tmp);
}

/* ------------------------------------------------------------------ */
/*  renderScreensaverPage — one large stat per page, with drift        */
/* ------------------------------------------------------------------ */

void UITask::renderScreensaverPage() {
  char tmp[80];
  int ox = _ss_offset_x;  // burn-in drift
  int oy = _ss_offset_y;

  _display->setColor(DisplayDriver::LIGHT);

  switch (_ss_page % SS_PAGES) {
    case 0: {
      // Node name + uptime
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 0 + oy, "Node / Uptime");

      uint32_t s = _stats.uptime_ms / 1000;
      uint32_t m = s / 60;  s %= 60;
      uint32_t h = m / 60;  m %= 60;
      uint32_t d = h / 24;  h %= 24;
      // Node name row
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 12 + oy, _node_prefs->node_name);
      // Uptime large
      snprintf(tmp, sizeof(tmp), "%lud%luh%lum", (unsigned long)d,
               (unsigned long)h, (unsigned long)m);
      _display->setTextSize(2);
      _display->drawTextCentered(64 + ox, 26 + oy, tmp);
      break;
    }
    case 1: {
      // Room count
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 0 + oy, "Active Rooms");
      snprintf(tmp, sizeof(tmp), "%d", (int)_stats.room_count);
      _display->setTextSize(2);
      _display->drawTextCentered(64 + ox, 22 + oy, tmp);
      break;
    }
    case 2: {
      // Total posts
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 0 + oy, "Total Posts");
      snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)_stats.total_posts);
      _display->setTextSize(2);
      _display->drawTextCentered(64 + ox, 22 + oy, tmp);
      break;
    }
    case 3: {
      // Radio settings (two lines of size-1 text, fits on small screen)
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 0 + oy, "Radio");
      snprintf(tmp, sizeof(tmp), "%.3f MHz SF%d", _node_prefs->freq, _node_prefs->sf);
      _display->drawTextCentered(64 + ox, 14 + oy, tmp);
      snprintf(tmp, sizeof(tmp), "BW%.1f CR4/%d", _node_prefs->bw, _node_prefs->cr);
      _display->drawTextCentered(64 + ox, 24 + oy, tmp);
      break;
    }
    case 4: {
      // WiFi status / IP
      _display->setTextSize(1);
      const char* mode_str = (_stats.wifi_mode == 1) ? "WiFi AP"
                           : (_stats.wifi_mode == 2) ? "WiFi STA"
                           : "WiFi OFF";
      _display->drawTextCentered(64 + ox, 0 + oy, mode_str);
      if (_stats.wifi_mode > 0 && _stats.wifi_ip[0]) {
        _display->setTextSize(1);
        _display->drawTextCentered(64 + ox, 14 + oy, _stats.wifi_ip);
      }
      break;
    }
    case 5: {
      // Known contacts
      _display->setTextSize(1);
      _display->drawTextCentered(64 + ox, 0 + oy, "Contacts");
      snprintf(tmp, sizeof(tmp), "%d", (int)_stats.contact_count);
      _display->setTextSize(2);
      _display->drawTextCentered(64 + ox, 22 + oy, tmp);
      break;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  CLI handler                                                         */
/* ------------------------------------------------------------------ */

void UITask::handleCommand(const char* cmd, char* reply) {
  // cmd starts with "screensaver"
  const char* arg = cmd + 11;
  while (*arg == ' ') arg++;

  if (strncmp(arg, "keep-on", 7) == 0) {
    const char* val = arg + 7;
    while (*val == ' ') val++;
    if (strcmp(val, "on") == 0) {
      setSsKeepOn(true);
      strcpy(reply, "OK - screensaver keep-on ON (screen stays on when ss disabled)");
    } else if (strcmp(val, "off") == 0) {
      setSsKeepOn(false);
      strcpy(reply, "OK - screensaver keep-on OFF (screen dims after 60s when ss disabled)");
    } else {
      snprintf(reply, 160, "screensaver=%s keep-on=%s",
               _ss_enabled ? "on" : "off", _ss_keep_on ? "on" : "off");
    }
  } else if (strncmp(arg, "interval", 8) == 0) {
    const char* val = arg + 8;
    while (*val == ' ') val++;
    int sec = atoi(val);
    if (sec >= 1 && sec <= 60) {
      setSsPageSec((uint8_t)sec);
      snprintf(reply, 160, "OK - screensaver interval set to %ds", sec);
    } else {
      snprintf(reply, 160, "screensaver=%s keep-on=%s interval=%ds (range: 1-60)",
               _ss_enabled ? "on" : "off", _ss_keep_on ? "on" : "off", (int)_ss_page_sec);
    }
  } else if (strcmp(arg, "on") == 0) {
    setSsEnabled(true);
    strcpy(reply, "OK - screensaver ON (cycling stats, keeps screen alive)");
  } else if (strcmp(arg, "off") == 0) {
    setSsEnabled(false);
    strcpy(reply, "OK - screensaver OFF");
  } else {
    // Status
    snprintf(reply, 160, "screensaver=%s keep-on=%s interval=%ds",
             _ss_enabled ? "on" : "off", _ss_keep_on ? "on" : "off", (int)_ss_page_sec);
  }
}

/* ------------------------------------------------------------------ */
/*  loop()                                                              */
/* ------------------------------------------------------------------ */

void UITask::loop() {
  unsigned long now = millis();
  bool is_boot = (now < BOOT_SCREEN_MILLIS);

  // Determine refresh interval:
  // - During boot animation: 50 ms (20 fps)
  // - In screensaver: 200 ms (smooth drift page advance)
  // - Home screen: 1000 ms
  unsigned long refresh_ms = is_boot ? 50UL : (_in_screensaver ? 200UL : 1000UL);

  // Button read (5 reads/s)
#ifdef PIN_USER_BTN
  if (now >= _next_read) {
    int btnState = digitalRead(PIN_USER_BTN);
    if (btnState != _prevBtnState) {
      if (btnState == USER_BTN_PRESSED) {
        if (_display->isOn()) {
          if (_in_screensaver) {
            _in_screensaver = false;  // exit screensaver back to home
          }
        } else {
          _display->turnOn();
        }
        _auto_off = now + AUTO_OFF_MILLIS;
      }
      _prevBtnState = btnState;
    }
    _next_read = now + 200;
  }
#endif

  if (!_display->isOn()) return;

  // Screensaver / auto-off state machine (only active post-boot)
  if (!is_boot) {
    if (now > _auto_off) {
      if (_ss_enabled) {
        if (!_in_screensaver) {
          // Enter screensaver
          _in_screensaver    = true;
          _ss_page           = 0;
          _ss_next_page      = now + (uint32_t)_ss_page_sec * 1000UL;
          _ss_offset_x       = SS_DRIFT_X[0];
          _ss_offset_y       = SS_DRIFT_Y[0];
        } else if (now >= _ss_next_page) {
          // Advance screensaver page
          _ss_page           = (_ss_page + 1) % SS_PAGES;
          _ss_next_page      = now + (uint32_t)_ss_page_sec * 1000UL;
          _ss_offset_x       = SS_DRIFT_X[_ss_page];
          _ss_offset_y       = SS_DRIFT_Y[_ss_page];
        }
      } else if (!_ss_keep_on) {
        // Screensaver off, keep-on off: turn off after extended timeout
        if (now > _auto_off + (KEEP_ON_DIM_MILLIS - AUTO_OFF_MILLIS)) {
          _display->turnOff();
          return;
        }
      }
      // else: ss=off, keep-on=on => stay on home screen indefinitely
    }
  }

  // Render frame
  if (now >= _next_refresh) {
    _display->startFrame();

    if (is_boot) {
      renderBootScreen();
    } else if (_in_screensaver) {
      renderScreensaverPage();
    } else {
      renderHomeScreen();
    }

    _display->endFrame();
    _next_refresh = now + refresh_ms;
  }
}
