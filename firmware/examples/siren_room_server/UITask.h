#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

// Stats populated by the main loop for the OLED screensaver.
// All fields are trivially copyable — no heap, no locks needed.
struct UiStats {
  uint8_t  room_count;      // active slot count
  uint32_t total_posts;     // total posts across all rooms
  uint32_t uptime_ms;       // millis() snapshot
  uint8_t  wifi_mode;       // 0=off, 1=AP, 2=STA
  char     wifi_ip[16];     // e.g. "192.168.4.1"
  uint8_t  contact_count;   // total known contacts across all rooms
};

class UITask {
  DisplayDriver*  _display;
  unsigned long   _next_read, _next_refresh, _auto_off;
  int             _prevBtnState;
  NodePrefs*      _node_prefs;
  char            _version_info[32];

  // Screensaver settings (persisted to SPIFFS /ss_cfg)
  bool            _ss_enabled;    // screensaver on/off (default: true)
  bool            _ss_keep_on;    // keep screen alive when ss=off (default: false)

  // Stats from main loop
  UiStats         _stats;

  // Screensaver runtime state
  bool            _in_screensaver;
  uint8_t         _ss_page;           // current stat page (0-5)
  unsigned long   _ss_next_page;      // when to advance to next page
  int8_t          _ss_offset_x;       // burn-in drift offset X
  int8_t          _ss_offset_y;       // burn-in drift offset Y

  void renderBootScreen();
  void renderHomeScreen();
  void renderScreensaverPage();

  void loadSsConfig();
  void saveSsConfig();

public:
  UITask(DisplayDriver& display)
    : _display(&display), _next_read(0), _next_refresh(0), _auto_off(0),
      _prevBtnState(0), _node_prefs(nullptr),
      _ss_enabled(true), _ss_keep_on(false),
      _in_screensaver(false), _ss_page(0), _ss_next_page(0),
      _ss_offset_x(0), _ss_offset_y(0)
  { memset(&_stats, 0, sizeof(_stats)); }

  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);

  /** Called by the main loop every iteration to update stats. Lightweight struct copy. */
  void setStats(const UiStats& s) { _stats = s; }

  bool isSsEnabled() const { return _ss_enabled; }
  bool isSsKeepOn()  const { return _ss_keep_on; }

  void setSsEnabled(bool v);
  void setSsKeepOn(bool v);

  /** Handle "screensaver ..." CLI command (serial + mesh CLI). */
  void handleCommand(const char* cmd, char* reply);

  void loop();
};
