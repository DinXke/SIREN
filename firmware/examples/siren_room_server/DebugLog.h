#pragma once
#include <Arduino.h>
#include <stdarg.h>

/* DebugLog — non-persistent RAM ring buffer for mesh debug events (JES-852).
 *
 * Disabled by default; enable via web UI toggle or CLI "debug on".
 * Entry layout: uint32_t ts + char[DEBUG_LOG_MSG_LEN] = 88 bytes each.
 * Default: 200 entries = ~17.6 KB static DRAM — acceptable on ESP32-S3
 *   (verified headroom: ~170 KB free at normal runtime RAM usage ~46-47%).
 *
 * Override at build time via platformio.ini build_flags:
 *   -D DEBUG_LOG_MAX_ENTRIES=100   (reduce to ~8.8 KB)
 *   -D DEBUG_LOG_MSG_LEN=60        (reduce per-entry size)
 */

#ifndef DEBUG_LOG_MAX_ENTRIES
  #define DEBUG_LOG_MAX_ENTRIES 200
#endif
#ifndef DEBUG_LOG_MSG_LEN
  #define DEBUG_LOG_MSG_LEN 84
#endif

struct DebugEntry {
  uint32_t ts;               // RTC timestamp (seconds since epoch, or millis/1000)
  char     msg[DEBUG_LOG_MSG_LEN];
};

class DebugLog {
  DebugEntry _buf[DEBUG_LOG_MAX_ENTRIES];
  uint16_t   _head;   // next-write index (ring)
  uint16_t   _count;  // entries stored (0..MAX_ENTRIES)
  bool       _enabled;

public:
  DebugLog() : _head(0), _count(0), _enabled(false) {
    memset(_buf, 0, sizeof(_buf));
  }

  /** Enable or disable logging.  Does NOT clear existing entries. */
  void enable(bool on)   { _enabled = on; }
  bool isEnabled() const { return _enabled; }

  /** Discard all entries and reset head pointer. */
  void clear() { _head = 0; _count = 0; }

  /** Append a formatted entry.  Silently no-ops when disabled. */
  void log(uint32_t ts, const char* fmt, ...) {
    if (!_enabled) return;
    DebugEntry& e = _buf[_head];
    e.ts = ts;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, DEBUG_LOG_MSG_LEN, fmt, ap);
    va_end(ap);
    _head = (uint16_t)((_head + 1) % DEBUG_LOG_MAX_ENTRIES);
    if (_count < DEBUG_LOG_MAX_ENTRIES) _count++;
  }

  /** Number of entries currently stored (0..MAX_ENTRIES). */
  uint16_t count() const { return _count; }

  /** Return entry by age index: 0 = oldest, count()-1 = newest. */
  const DebugEntry& get(uint16_t i) const {
    uint16_t idx;
    if (_count < DEBUG_LOG_MAX_ENTRIES) {
      idx = i % DEBUG_LOG_MAX_ENTRIES;
    } else {
      idx = (uint16_t)((_head + (uint32_t)i) % DEBUG_LOG_MAX_ENTRIES);
    }
    return _buf[idx];
  }

  /** RAM used by this instance in bytes. */
  static constexpr size_t ramBytes() {
    return sizeof(DebugEntry) * DEBUG_LOG_MAX_ENTRIES;
  }
};

/* Global singleton — defined in MyMesh.cpp */
extern DebugLog g_dbglog;

/* DLOG(ts, fmt, ...) — appends one entry; ts is a uint32_t RTC timestamp.
 * Zero overhead when logging is disabled (function-level check). */
#define DLOG(ts, ...) g_dbglog.log((ts), __VA_ARGS__)
