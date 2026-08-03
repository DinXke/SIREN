#include "MyMesh.h"
#include "DebugLog.h"
#include <algorithm>  // std::sort — neighbour list ordering (JES-869)
#ifdef ESP32
#include <esp_system.h>  // esp_fill_random() — SEC-001 first-boot password CSPRNG
#endif

/* JES-852: RAM debug log singleton — 200 entries × 88 B = ~17.6 KB DRAM.
   Enable via web UI (Systeem > Debug Log) or serial CLI: debug on */
DebugLog g_dbglog;

/* ------------------------------------------------------------------ */
/*  Timing constants                                                    */
/* ------------------------------------------------------------------ */
#define REPLY_DELAY_MILLIS        1500
#define PUSH_NOTIFY_DELAY_MILLIS  2000
#define SYNC_PUSH_INTERVAL        1200

#define PUSH_ACK_TIMEOUT_FLOOD    12000
#define PUSH_TIMEOUT_BASE          4000
#define PUSH_ACK_TIMEOUT_FACTOR    2000

#define POST_SYNC_DELAY_SECS       6
#define LAZY_CONTACTS_WRITE_DELAY  5000

#define LOCAL_ADVERT_INTERVAL_MS   (2UL * 60 * 1000)   // 2 min
#define FLOOD_ADVERT_INTERVAL_MS   (47UL * 60 * 60 * 1000)  // 47 h

/* ------------------------------------------------------------------ */
/*  REQ / RESP type codes (same as simple_room_server)                  */
/* ------------------------------------------------------------------ */
#define FIRMWARE_VER_LEVEL          1

#define REQ_TYPE_GET_STATUS         0x01
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05

#define RESP_SERVER_LOGIN_OK        0

/* ------------------------------------------------------------------ */
/*  Constructor                                                         */
/* ------------------------------------------------------------------ */
MultiRoomMesh::MultiRoomMesh(mesh::MainBoard& board, mesh::Radio& radio,
                             mesh::MillisecondClock& ms, mesh::RNG& rng,
                             mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, rooms[0].acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      _discover_limiter(4, 120)  // max 4 discover-responses per 2 minutes (JES-869)
{
  _fs = nullptr;
  _active_slot = 0;
  _num_active_rooms = 0;
  last_millis = 0;
  uptime_millis = 0;
  _logging = false;
  region_load_active = false;
  set_radio_at = revert_radio_at = 0;
  _web_syncreq_pending = _web_roomsync_pending = false;
  _web_syncreq_idx = _web_roomsync_idx = -1;
  _web_fullsync_pending = false;
  _web_fullsync_idx = -1;
  _web_advert_pending = false;
  _web_discover_pending = false;
  _pending_discover_tag = 0;
  _pending_discover_until = 0;
#if MAX_NEIGHBOURS
  memset(_neighbours, 0, sizeof(_neighbours));
#endif
  memset((void*)_rxlog, 0, sizeof(_rxlog));
  _rxlog_head = 0;
  _rxlog_total = 0;
  _post_dirty_at = 0;
  _mqtt_post_cb  = nullptr;
  _mqtt_post_ctx = nullptr;

  memset(_names, 0, sizeof(_names));
  _name_lru_ctr = 0;

  memset(_dm_convs, 0, sizeof(_dm_convs));
  _dm_num_convs = 0;

  memset(_tombstones, 0, sizeof(_tombstones));
  _tombstone_count = 0;

  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor       = 1.0f;
  _prefs.rx_delay_base        = 0.0f;
  _prefs.tx_delay_factor      = 0.5f;
  _prefs.direct_tx_delay_factor = 0.2f;
  _prefs.freq         = LORA_FREQ;
  _prefs.sf           = LORA_SF;
  _prefs.bw           = LORA_BW;
  _prefs.cr           = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.disable_fwd  = 0;  // JES-842: flood forwarding ON by default
  _prefs.advert_interval       = 1;   // 2 min
  _prefs.flood_advert_interval = 47;  // 47 h
  _prefs.flood_max         = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert  = 8;
  _prefs.interference_threshold = 0;
  StrHelper::strncpy(_prefs.node_name, "SIREN", sizeof(_prefs.node_name));
  _prefs.password[0] = 0;  // empty; randomised or loaded from SPIFFS in begin()

  memset(default_scope.key, 0, sizeof(default_scope.key));
  memset(rooms, 0, sizeof(rooms));
  memset(_post_pool, 0, sizeof(_post_pool));
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) _post_pool[i].room_idx = 0xFF;
  memset(peers, 0, sizeof(peers));
  _num_peers = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    peers[i].secret_valid = false;
    peers[i].next_sync_at = 0;
  }
  _sync_req_sent = _sync_dat_recv = _sync_posts_recv = _sync_posts_sent = 0;
  memset(_last_login_notify_ms,  0, sizeof(_last_login_notify_ms));
  memset(_notify_targets,        0, sizeof(_notify_targets));
  memset(_notify_target_count,   0, sizeof(_notify_target_count));
  memset(_hist_ring, 0, sizeof(_hist_ring));
  _hist_head      = 0;
  _hist_bucket_ts = 0;
  _advert_interval_sec = 120;
  _sync_interval_s     = 180;

  for (int i = 0; i < MAX_ROOMS; i++) {
    rooms[i].active          = false;
    rooms[i].stealth         = true;  // default: no adverts (stealth by default)
    rooms[i].next_client_idx = 0;
    rooms[i].num_posted      = 0;
    rooms[i].num_post_pushes = 0;
    rooms[i].next_push       = 0;
    rooms[i].next_local_advert  = 0;
    rooms[i].next_flood_advert  = 0;
    rooms[i].dirty_contacts_expiry = 0;
    rooms[i].lat = 0.0f;
    rooms[i].lon = 0.0f;
  }
}

/* ------------------------------------------------------------------ */
/*  begin()                                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::begin(FILESYSTEM* fs) {
  mesh::Mesh::begin();
  _fs = fs;

#ifdef ESP32
  // Create mutex that guards rooms[] against concurrent access from the
  // AsyncTCP web-handler task (Core 0) and the main-loop LoRa task (Core 1).
  // Must be done before loadRoomConfig() so any early accessor calls are safe.  (JES-865)
  _rooms_mutex = xSemaphoreCreateMutex();
#endif

  _cli.loadPrefs(_fs);

  // JES-842: room server must always forward flood packets. Older firmware had
  // disable_fwd=1 hardcoded and that value gets saved to /com_prefs. Force it
  // back to 0 and re-save so OTA upgrades from old firmware self-heal on first boot.
  if (_prefs.disable_fwd != 0) {
    _prefs.disable_fwd = 0;
    _cli.savePrefs(_fs);
    Serial.printf("[SIREN] JES-842: flood forwarding was disabled — re-enabled and saved\n");
  }

  // SEC-001: ensure admin password is never the well-known default "password".
  // On first boot _prefs.password is empty (constructor) or may have been
  // set to the legacy "password" by a build flag.  Both cases trigger a random
  // password that is persisted immediately so all subsequent boots load it.
  {
    bool needs_rand = (_prefs.password[0] == 0 ||
                       strcmp(_prefs.password, "password") == 0);
#ifdef ADMIN_PASSWORD
    // Operator supplied a custom password at build time — honour it unless it
    // is the unsafe well-known default.
    if (!needs_rand) {
      // already loaded a non-default password from prefs — keep it
    } else if (strcmp(ADMIN_PASSWORD, "password") != 0) {
      // operator-supplied non-default → use it
      StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
      _cli.savePrefs(_fs);
      needs_rand = false;
    }
#endif
    if (needs_rand) {
      // Generate a random 10-char password using ESP32 hardware CSPRNG.
      static const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";  // 32 chars, no ambiguous
      uint8_t rnd[10];
      esp_fill_random(rnd, sizeof(rnd));
      for (int i = 0; i < 10; i++)
        _prefs.password[i] = charset[rnd[i] & 0x1F];  // 2^5 = 32
      _prefs.password[10] = 0;
      _cli.savePrefs(_fs);
      Serial.printf("[SIREN] First-boot admin password: %s\n", _prefs.password);
      Serial.printf("[SIREN] Change via web UI > Settings or serial CLI: password <new>\n");
    }
  }

#ifdef FORCE_RADIO_PREFS
  // One-shot radio settings correction: overwrite any stale prefs from a
  // previous firmware's SPIFFS while preserving node name, identity, and all
  // non-radio settings.  After this boot the corrected values are persisted so
  // a subsequent normal OTA (without FORCE_RADIO_PREFS) will load them cleanly.
  _prefs.freq        = LORA_FREQ;
  _prefs.sf          = LORA_SF;
  _prefs.bw          = LORA_BW;
  _prefs.cr          = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _cli.savePrefs(_fs);
  Serial.printf("[SIREN] FORCE_RADIO_PREFS: freq=%.3f sf=%d bw=%.1f cr=%d tx=%d — saved\n",
                _prefs.freq, _prefs.sf, _prefs.bw, _prefs.cr, _prefs.tx_power_dbm);
#endif

  // Load or create room identities + config
  loadRoomConfig();   // loads name/password overrides from /room_cfg
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].active) {
      loadOrCreateRoomIdentity(i);
    }
  }
  // Ensure at least one room is active
  if (_num_active_rooms == 0) {
    // Create default room 0
    rooms[0].active = true;
    StrHelper::strncpy(rooms[0].name, _prefs.node_name, sizeof(rooms[0].name));
    StrHelper::strncpy(rooms[0].password, _prefs.password, sizeof(rooms[0].password));
    _num_active_rooms = 1;
    loadOrCreateRoomIdentity(0);
    saveRoomConfig();
  }

  loadPeerConfig();
  loadSyncConfig();    // restore configurable anti-entropy interval (JES-844)
  // Schedule staggered initial sync for each configured peer (Phase 5)
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active) {
      peers[i].next_sync_at     = futureMillis(PEER_SYNC_BOOT_DELAY_MS + (uint32_t)i * 5000);
      peers[i].next_roomsync_at = futureMillis(PEER_SYNC_BOOT_DELAY_MS + (uint32_t)i * 5000 + 30000UL);
    }
  }
  loadPostPool();      // restore persisted messages (JES-787)
  loadTombstones();     // restore delete tombstones (JES-824)
  loadNameTable();      // restore advertised-name cache (JES-798)
  loadNotifyTargets();  // restore login notification targets (JES-834)

  region_map.load(_fs);

  // Establish default scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    }
  }

  // Apply radio settings
  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  // Stagger initial advert timers per room to avoid collision.
  // Stealth rooms keep timers at 0 (disabled) until visibility is enabled.
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (!rooms[i].stealth) {
      uint32_t offset_ms = (uint32_t)i * 15000;  // 15 s stagger
      rooms[i].next_local_advert = futureMillis(LOCAL_ADVERT_INTERVAL_MS + offset_ms);
      rooms[i].next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + offset_ms);
    }
  }

  board.setAdcMultiplier(_prefs.adc_multiplier);

  // Use room[0] as the primary self_id for the Mesh base
  self_id = rooms[0].id;

  Serial.printf("[SIREN] %d room(s) active\n", _num_active_rooms);
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    Serial.printf("  room[%d] name='%s' id=", i, rooms[i].name);
    mesh::Utils::printHex(Serial, rooms[i].id.pub_key, PUB_KEY_SIZE);
    Serial.println();
  }
}

/* ------------------------------------------------------------------ */
/*  Identity persistence                                                */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::loadOrCreateRoomIdentity(int idx) {
#if defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
  store.begin();
#elif defined(NRF52_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  #error "Unsupported platform"
#endif

  char key[16];
  snprintf(key, sizeof(key), "_room%d", idx);

  // Backward-compat: if _room0 not found, fall back to legacy "_main" key
  bool loaded = store.load(key, rooms[idx].id);
  if (!loaded && idx == 0) {
    loaded = store.load("_main", rooms[idx].id);
    if (loaded) store.save(key, rooms[idx].id);  // migrate to _room0
  }

#if defined(SIREN_DEFAULT_PRV_KEY_HEX) && defined(SIREN_FORCE_DEFAULT_PRV_KEY)
  // Provisioning mode: always overwrite room 0 with the baked-in key.
  // Use this flag only for initial provisioning builds — remove for production.
  if (idx == 0) {
    uint8_t prv[64];
    if (mesh::Utils::fromHex(prv, 64, SIREN_DEFAULT_PRV_KEY_HEX) &&
        mesh::LocalIdentity::validatePrivateKey(prv)) {
      rooms[idx].id.readFrom(prv, 64);
      store.save(key, rooms[idx].id);
      loaded = true;
    }
  }
#endif

  if (!loaded) {
    bool loaded_baked = false;
#if defined(SIREN_DEFAULT_PRV_KEY_HEX)
    if (idx == 0) {
      uint8_t prv[64];
      if (mesh::Utils::fromHex(prv, 64, SIREN_DEFAULT_PRV_KEY_HEX) &&
          mesh::LocalIdentity::validatePrivateKey(prv)) {
        rooms[idx].id.readFrom(prv, 64);
        loaded_baked = true;
      }
      // else: baked key invalid or wrong length — fall through to random
    }
#endif
    if (!loaded_baked) {
      rooms[idx].id = radio_new_identity();
      int attempts = 0;
      while (attempts < 10 &&
             (rooms[idx].id.pub_key[0] == 0x00 ||
              rooms[idx].id.pub_key[0] == 0xFF)) {
        rooms[idx].id = radio_new_identity();
        attempts++;
      }
    }
    store.save(key, rooms[idx].id);
  }
}

void MultiRoomMesh::saveRoomIdentity(int idx) {
#if defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#elif defined(NRF52_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  #error "Unsupported platform"
#endif

  char key[16];
  snprintf(key, sizeof(key), "_room%d", idx);
  store.save(key, rooms[idx].id);
}

/* JES-821: Rotate private key for room idx.
   - Generates a new keypair (with 0x00/0xFF retry-guard, same as initial key gen).
   - Persists immediately to IdentityStore.
   - Room 0: updates self_id and invalidates all peer ECDH shared secrets.
   - Resets VV (we are now a new origin; old VV entries are stale).
   - Schedules an advert shortly if room is not stealth (JES-772).
   SECURITY: never logs or returns the private key. */
void MultiRoomMesh::rekeyRoom(int idx) {
  if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) return;

  // Generate new keypair with 0x00/0xFF retry-guard
  rooms[idx].id = radio_new_identity();
  int attempts = 0;
  while (attempts < 10 &&
         (rooms[idx].id.pub_key[0] == 0x00 ||
          rooms[idx].id.pub_key[0] == 0xFF)) {
    rooms[idx].id = radio_new_identity();
    attempts++;
  }

  // Persist to flash immediately
  saveRoomIdentity(idx);

  if (idx == 0) {
    // Room 0 = node identity: update self_id and invalidate all peer ECDH secrets
    self_id = rooms[0].id;
    for (int pi = 0; pi < MAX_PEERS; pi++) {
      peers[pi].secret_valid = false;  // force ECDH recalc on next sync
    }
  }

  // Reset VV — we are now a different origin; old VV entries are stale
  memset(rooms[idx].vv, 0, sizeof(rooms[idx].vv));

  // Schedule advert soon if NOT stealth (JES-772)
  if (!rooms[idx].stealth) {
    rooms[idx].next_local_advert = futureMillis(500);
    rooms[idx].next_flood_advert = futureMillis(500);
  }

  // JES-856: propagate new key to all peers immediately
  triggerRoomSync(-1);
}

/* Room config: simple binary layout
   [1 byte: num_rooms] then for each room:
     [1 byte: active][24: name][16: password][16: guest_password]
     [4: lat float][4: lon float]
*/
#define ROOM_CFG_PATH "/room_cfg"

void MultiRoomMesh::saveRoomConfig() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(ROOM_CFG_PATH, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(ROOM_CFG_PATH);
  File f = _fs->open(ROOM_CFG_PATH, FILE_O_WRITE);
#else
  File f = _fs->open(ROOM_CFG_PATH, "w", true);
#endif
  if (!f) return;

  uint8_t n = MAX_ROOMS;
  f.write(&n, 1);
  for (int i = 0; i < MAX_ROOMS; i++) {
    uint8_t active      = rooms[i].active  ? 1 : 0;
    uint8_t stealth_b   = rooms[i].stealth ? 1 : 0;
    f.write(&active, 1);
    f.write((uint8_t*)rooms[i].name,          sizeof(rooms[i].name));
    f.write((uint8_t*)rooms[i].password,      sizeof(rooms[i].password));
    f.write((uint8_t*)rooms[i].guest_password,sizeof(rooms[i].guest_password));
    f.write((uint8_t*)&rooms[i].lat, 4);
    f.write((uint8_t*)&rooms[i].lon, 4);
    f.write(&stealth_b, 1);   // appended last for backward compat
  }
  // Global advert interval (seconds) — appended after all rooms for backward compat
  uint16_t ais = _advert_interval_sec;
  f.write((uint8_t*)&ais, 2);
  f.close();
}

void MultiRoomMesh::loadRoomConfig() {
#if defined(RP2040_PLATFORM)
  if (!_fs->exists(ROOM_CFG_PATH)) return;
  File f = _fs->open(ROOM_CFG_PATH, "r");
#else
  if (!_fs->exists(ROOM_CFG_PATH)) return;
  File f = _fs->open(ROOM_CFG_PATH);
#endif
  if (!f) return;

  uint8_t n = 0;
  if (f.read(&n, 1) != 1) { f.close(); return; }
  if (n > MAX_ROOMS) n = MAX_ROOMS;

  _num_active_rooms = 0;
  for (int i = 0; i < n; i++) {
    uint8_t active = 0;
    if (f.read(&active, 1) != 1) break;
    if (f.read((uint8_t*)rooms[i].name,           sizeof(rooms[i].name))          != (int)sizeof(rooms[i].name))  break;
    if (f.read((uint8_t*)rooms[i].password,        sizeof(rooms[i].password))      != (int)sizeof(rooms[i].password)) break;
    if (f.read((uint8_t*)rooms[i].guest_password,  sizeof(rooms[i].guest_password))!= (int)sizeof(rooms[i].guest_password)) break;
    if (f.read((uint8_t*)&rooms[i].lat, 4) != 4) break;
    if (f.read((uint8_t*)&rooms[i].lon, 4) != 4) break;
    // Stealth byte — appended in v2 of this format; default=1 (stealth) on EOF
    uint8_t stealth_b = 1;
    f.read(&stealth_b, 1);  // ignore return; default holds on short read

    rooms[i].active  = (active != 0);
    rooms[i].stealth = (stealth_b != 0);
    if (rooms[i].active) _num_active_rooms++;
  }
  // Global advert interval — appended after all rooms; default=120 on EOF
  uint16_t ais = 120;
  f.read((uint8_t*)&ais, 2);  // ignore return; default holds on short read
  if (ais < 10 || ais > 64800) ais = 120;
  _advert_interval_sec = ais;
  f.close();
}

/* ------------------------------------------------------------------ */
/*  Stealth: visibility control per-room or all-rooms                  */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::setRoomStealth(int idx, bool s) {
  int first = (idx < 0) ? 0 : idx;
  int last  = (idx < 0) ? MAX_ROOMS - 1 : idx;
  for (int i = first; i <= last; i++) {
    if (idx >= 0 && i != idx) continue;
    if (i < 0 || i >= MAX_ROOMS) continue;
    rooms[i].stealth = s;
    if (s) {
      // Going stealth: disable advert timers
      rooms[i].next_local_advert = 0;
      rooms[i].next_flood_advert = 0;
    } else if (rooms[i].active) {
      // Becoming visible: schedule first advert soon (staggered per slot)
      uint32_t offset_ms = (uint32_t)i * 3000;
      rooms[i].next_local_advert = futureMillis(5000 + offset_ms);
      rooms[i].next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + offset_ms);
    }
  }
  saveRoomConfig();
}

/* ------------------------------------------------------------------ */
/*  Advert interval: global local advert period in seconds             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::setAdvertIntervalSec(uint16_t sec) {
  if (sec < 10) sec = 10;
  if (sec > 64800) sec = 64800;   // 18 h — board sets this in hours via the web UI
  _advert_interval_sec = sec;
  // Reschedule local advert timers for all visible active rooms
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active || rooms[i].stealth) continue;
    uint32_t offset_ms = (uint32_t)i * 3000;
    rooms[i].next_local_advert = futureMillis((uint32_t)_advert_interval_sec * 1000 + offset_ms);
  }
  saveRoomConfig();
}

/* ------------------------------------------------------------------ */
/*  Flood advert interval (hours) — separate from zero-hop (JES-868)   */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::setFloodAdvertIntervalHours(uint8_t hours) {
  if (hours > 240) hours = 240;   // uint8 cap; 0 = disable flood adverts
  _prefs.flood_advert_interval = hours;
  updateFloodAdvertTimer();       // reschedules all visible rooms (0 => disabled)
  savePrefs();                    // persisted by CommonCLI prefs file
}

/* ------------------------------------------------------------------ */
/*  Sync interval: configurable anti-entropy pull period (JES-844)     */
/* ------------------------------------------------------------------ */
#define SYNC_CFG_PATH "/sync_cfg"

void MultiRoomMesh::setSyncIntervalSec(uint32_t sec) {
  if (sec < 10)   sec = 10;
  if (sec > 3600) sec = 3600;
  _sync_interval_s = sec;
  // Reschedule pending peer sync timers to respect new interval immediately
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].active) continue;
    peers[i].next_sync_at = futureMillis((uint32_t)_sync_interval_s * 1000);
  }
  saveSyncConfig();
}

void MultiRoomMesh::saveSyncConfig() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(SYNC_CFG_PATH, "w");
#else
  File f = _fs->open(SYNC_CFG_PATH, "w");
#endif
  if (!f) return;
  f.write((uint8_t*)&_sync_interval_s, 4);
  f.close();
}

void MultiRoomMesh::loadSyncConfig() {
  if (!_fs->exists(SYNC_CFG_PATH)) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(SYNC_CFG_PATH, "r");
#else
  File f = _fs->open(SYNC_CFG_PATH);
#endif
  if (!f) return;
  uint32_t sec = 180;
  f.read((uint8_t*)&sec, 4);
  f.close();
  if (sec < 10 || sec > 3600) sec = 180;
  _sync_interval_s = sec;
}

/* ------------------------------------------------------------------ */
/*  onRecvPacket — multi-room routing override                          */
/* ------------------------------------------------------------------ */
mesh::DispatcherAction MultiRoomMesh::onRecvPacket(mesh::Packet* pkt) {
  uint8_t ptype = pkt->getPayloadType();

  // Live-view: record metadata of EVERY received packet (JES-868), incl. flood
  // packets not addressed to this node. Metadata only — never the payload.
  recordRxLog(pkt);

  // For addressed packets (anon login / peer data / path), find the matching room
  if (pkt->payload_len >= 1 &&
      (ptype == PAYLOAD_TYPE_ANON_REQ  ||
       ptype == PAYLOAD_TYPE_PATH      ||
       ptype == PAYLOAD_TYPE_REQ       ||
       ptype == PAYLOAD_TYPE_RESPONSE  ||
       ptype == PAYLOAD_TYPE_TXT_MSG)) {

    uint8_t dest_hash = pkt->payload[0];

    for (int s = 0; s < MAX_ROOMS; s++) {
      if (!rooms[s].active) continue;
      if (rooms[s].id.isHashMatch(&dest_hash)) {
        _active_slot = s;
        self_id = rooms[s].id;   // swap so Mesh base uses correct key
        return mesh::Mesh::onRecvPacket(pkt);
      }
    }
    // No room matched — let parent handle (will be dropped after hash mismatch)
    _active_slot = 0;
    self_id = rooms[0].id;
    return mesh::Mesh::onRecvPacket(pkt);
  }

  // Non-addressed packets (ACK, ADVERT, flood routing): use room[0] context
  _active_slot = 0;
  self_id = rooms[0].id;
  return mesh::Mesh::onRecvPacket(pkt);
}

/* ------------------------------------------------------------------ */
/*  searchPeersByHash / getPeerSharedSecret                             */
/* ------------------------------------------------------------------ */
int MultiRoomMesh::searchPeersByHash(const uint8_t* hash) {
  ClientACL& acl = rooms[_active_slot].acl;
  int n = 0;
  // ACL clients: stored as non-negative index
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i;
      if (n >= MAX_CLIENTS) break;
    }
  }
  // Phase 5: peer room-servers — stored as -(pi+1) (negative sentinel)
  for (int i = 0; i < MAX_PEERS && n < MAX_CLIENTS; i++) {
    if (!peers[i].active) continue;
    if (memcmp(hash, peers[i].pub_key, 1) == 0) {  // 1-byte hash match
      matching_peer_indexes[n++] = -(i + 1);
    }
  }
  return n;
}

void MultiRoomMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  int raw = matching_peer_indexes[peer_idx];
  if (raw < 0) {
    // Peer room-server: compute ECDH(rooms[0].priv, peer.pub) on first use
    int pi = -raw - 1;
    calcPeerSecret(pi);
    memcpy(dest_secret, peers[pi].shared_secret, PUB_KEY_SIZE);
  } else {
    ClientACL& acl = rooms[_active_slot].acl;
    if (raw < acl.getNumClients()) {
      memcpy(dest_secret, acl.getClientByIdx(raw)->shared_secret, PUB_KEY_SIZE);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  _notifyAdminsLoginAttempt — P2P DM to configured notify targets on login (JES-834) */
/* ------------------------------------------------------------------------------- */
void MultiRoomMesh::_notifyAdminsLoginAttempt(
    int slot_idx, const uint8_t* caller_pubkey, bool success) {

  if (_notify_target_count[slot_idx] == 0) return;

  uint32_t now_ms = millis();
  if ((now_ms - _last_login_notify_ms[slot_idx]) < 30000) return;  // rate limit
  _last_login_notify_ms[slot_idx] = now_ms;

  char pub_hex[9];
  snprintf(pub_hex, sizeof(pub_hex), "%02x%02x%02x%02x",
           caller_pubkey[0], caller_pubkey[1],
           caller_pubkey[2], caller_pubkey[3]);

  char msg[80];
  snprintf(msg, sizeof(msg), "Login %s op '%s': <%s>",
           success ? "OK" : "poging",
           rooms[slot_idx].name, pub_hex);
  int msg_len = strlen(msg);

  uint32_t now_rtc = getRTCClock()->getCurrentTimeUnique();

  // Send as P2P DM from rooms[0].id (node identity) to each configured target.
  // The companion can decrypt using ECDH(companion_priv, rooms[0].pub_key) and
  // will show this as a direct message, not a room chat message.
  for (int i = 0; i < _notify_target_count[slot_idx]; i++) {
    const uint8_t* target_pub = _notify_targets[slot_idx][i];

    uint8_t shared[PUB_KEY_SIZE];
    rooms[0].id.calcSharedSecret(shared, target_pub);

    mesh::Identity target_id;
    memset(target_id.pub_key, 0, PUB_KEY_SIZE);
    memcpy(target_id.pub_key, target_pub, PUB_KEY_SIZE);

    uint8_t buf[5 + 80];
    memcpy(buf, &now_rtc, 4);
    buf[4] = (TXT_TYPE_PLAIN << 2);
    memcpy(&buf[5], msg, msg_len);

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, target_id, shared,
                              buf, 5 + msg_len);
    if (pkt) sendFloodScoped(default_scope, pkt, 500, _prefs.path_hash_mode + 1);
  }
  self_id = rooms[slot_idx].id;  // restore for onAnonDataRecv continuation
}

/* ------------------------------------------------------------------ */
/*  onAnonDataRecv — handles login (ANON_REQ)                          */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                   const mesh::Identity& sender,
                                   uint8_t* data, size_t len) {
  if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;
  // SEC-002: guard against truncated ANON_REQ (pre-auth path, must be ≥9 bytes:
  // 4-byte timestamp + 4-byte sync_since + at least NUL terminator of password)
  if (len < 9) return;

  RoomSlot& slot = rooms[_active_slot];

  uint32_t sender_timestamp, sender_sync_since;
  memcpy(&sender_timestamp,   data,      4);
  memcpy(&sender_sync_since, &data[4],   4);
  data[len] = 0;

  DLOG(sender_timestamp,
       "RX anon room=%d sender=%02x%02x%02x%02x since=%lu len=%d",
       (int)_active_slot,
       (unsigned)sender.pub_key[0], (unsigned)sender.pub_key[1],
       (unsigned)sender.pub_key[2], (unsigned)sender.pub_key[3],
       (unsigned long)sender_sync_since, (int)len);

  ClientInfo* client = nullptr;

  if (data[8] == 0) {
    // Blank password — check if sender is already in ACL
    client = slot.acl.getClient(sender.pub_key, PUB_KEY_SIZE);
  }

  if (client == nullptr) {
    uint8_t perm;
    if (strcmp((char*)&data[8], slot.password) == 0) {
      perm = PERM_ACL_ADMIN;
    } else if (slot.guest_password[0] &&
               strcmp((char*)&data[8], slot.guest_password) == 0) {
      perm = PERM_ACL_READ_WRITE;
    } else if (_prefs.allow_read_only) {
      perm = PERM_ACL_GUEST;
    } else {
      MESH_DEBUG_PRINTLN("room[%d] incorrect password", (uint32_t)_active_slot);
      _notifyAdminsLoginAttempt(_active_slot, sender.pub_key, false);
      return;
    }

    client = slot.acl.putClient(sender, 0);
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("room[%d] possible replay attack", (uint32_t)_active_slot);
      return;
    }

    client->last_timestamp          = sender_timestamp;
    client->extra.room.sync_since   = sender_sync_since;
    client->extra.room.pending_ack  = 0;
    client->extra.room.push_failures = 0;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perm;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    slot.dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    _notifyAdminsLoginAttempt(_active_slot, sender.pub_key, true);
  }

  if (packet->isRouteFlood()) {
    client->out_path_len = OUT_PATH_UNKNOWN;
  }

  // Update per-client radio stats (JES-800)
  client->last_rssi = (int8_t)radio_driver.getLastRSSI();
  client->last_snr  = (int8_t)(radio_driver.getLastSNR() * 4);

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;
  reply_data[6] = (client->isAdmin() ? 1 : (client->permissions == 0 ? 2 : 0));
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);
  reply_data[12] = FIRMWARE_VER_LEVEL;

  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);

  if (packet->isRouteFlood()) {
    mesh::Packet* path = createPathReturn(sender, client->shared_secret,
                                          packet->path, packet->path_len,
                                          PAYLOAD_TYPE_RESPONSE, reply_data, 13);
    if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
  } else {
    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender,
                                          client->shared_secret, reply_data, 13);
    if (reply) {
      if (client->out_path_len != OUT_PATH_UNKNOWN) {
        sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
      } else {
        sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  onPeerDataRecv                                                      */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type,
                                   int sender_idx, const uint8_t* secret,
                                   uint8_t* data, size_t len) {
  int raw = matching_peer_indexes[sender_idx];

  // Phase 5: peer room-server DM (SYNCREQ / SYNCDAT / SYNCEND)
  if (raw < 0) {
    int pi = -raw - 1;
    if (pi < 0 || pi >= MAX_PEERS || !peers[pi].active) return;
    peers[pi].last_contact = getRTCClock()->getCurrentTime();
    if (type == PAYLOAD_TYPE_TXT_MSG && len > 4) {
      uint8_t flags = (data[4] >> 2);
      DLOG(peers[pi].last_contact,
           "RX peer[%d] type=%d flags=%d len=%d",
           pi, (int)type, (int)flags, (int)len);
      if      (flags == TXT_TYPE_SYNCREQ)  handleSyncReq(pi, data, len);
      else if (flags == TXT_TYPE_SYNCDAT)  handleSyncDat(pi, data, len);
      else if (flags == TXT_TYPE_SYNCDAT2) handleSyncDat(pi, data, len);  // JES-861: same handler, carries msg_id
      else if (flags == TXT_TYPE_SYNCEND)  handleSyncEnd(pi, data, len);
      else if (flags == TXT_TYPE_SYNCDEL)  handleSyncDel(pi, data, len);
      else if (flags == TXT_TYPE_ROOMSYNC) handleRoomSync(pi, data, len);
    }
    return;
  }

  RoomSlot& slot = rooms[_active_slot];
  int i = raw;
  if (i < 0 || i >= slot.acl.getNumClients()) return;

  ClientInfo* client = slot.acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    uint8_t flags = (data[4] >> 2);

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) return;

    if (sender_timestamp >= client->last_timestamp) {
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;

      uint32_t now = getRTCClock()->getCurrentTimeUnique();
      client->last_activity = now;
      client->extra.room.push_failures = 0;
      // Update per-client radio stats (JES-800)
      client->last_rssi = (int8_t)radio_driver.getLastRSSI();
      client->last_snr  = (int8_t)(radio_driver.getLastSNR() * 4);

      data[len] = 0;

      uint32_t ack_hash;
      mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 5 + strlen((char*)&data[5]),
                           client->id.pub_key, PUB_KEY_SIZE);

      uint8_t temp[166];
      bool send_ack = false;

      if (flags == TXT_TYPE_CLI_DATA) {
        if (client->isAdmin()) {
          if (!is_retry) {
            handleCommand(sender_timestamp, (char*)&data[5], (char*)&temp[5]);
            temp[4] = (TXT_TYPE_CLI_DATA << 2);
          } else {
            temp[5] = 0;
          }
        } else {
          temp[5] = 0;
        }
      } else {
        if ((client->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
          temp[5] = 0;
        } else {
          if (!is_retry) {
            // JES-861: tag the post with the sender's message timestamp so a
            // copy flooded to a coupled node dedups to a single displayed post.
            addPost(slot, client, (const char*)&data[5], sender_timestamp);
            dmBuffer(client->id.pub_key, sender_timestamp, false, (const char*)&data[5]);
          }
          temp[5] = 0;
          send_ack = true;
        }
      }

      uint32_t delay_millis = 0;
      if (send_ack) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          mesh::Packet* ack = createAck(ack_hash);
          if (ack) sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          delay_millis = TXT_ACK_DELAY + REPLY_DELAY_MILLIS;
        } else {
          uint32_t d = TXT_ACK_DELAY;
          if (getExtraAckTransmitCount() > 0) {
            mesh::Packet* a1 = createMultiAck(ack_hash, 1);
            if (a1) sendDirect(a1, client->out_path, client->out_path_len, d);
            d += 300;
          }
          mesh::Packet* a2 = createAck(ack_hash);
          if (a2) sendDirect(a2, client->out_path, client->out_path_len, d);
          delay_millis = d + REPLY_DELAY_MILLIS;
        }
      }

      int text_len = strlen((char*)&temp[5]);
      if (text_len > 0) {
        if (now == sender_timestamp) now++;
        memcpy(temp, &now, 4);
        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, delay_millis + SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, delay_millis + SERVER_RESPONSE_DELAY);
          }
        }
      }
    }
  } else if (type == PAYLOAD_TYPE_REQ && len >= 5) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    if (sender_timestamp < client->last_timestamp) return;

    client->last_timestamp = sender_timestamp;
    uint32_t now = getRTCClock()->getCurrentTime();
    client->last_activity = now;
    client->extra.room.push_failures = 0;

    if (data[4] == REQ_TYPE_KEEP_ALIVE && packet->isRouteDirect()) {
      uint32_t forceSince = 0;
      if (len >= 9) {
        memcpy(&forceSince, &data[5], 4);
      } else {
        memset(&data[5], 0, 4);
      }
      if (forceSince > 0) client->extra.room.sync_since = forceSince;
      client->extra.room.pending_ack = 0;

      if (client->out_path_len != OUT_PATH_UNKNOWN) {
        uint32_t ack_hash;
        mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 9, client->id.pub_key, PUB_KEY_SIZE);
        auto reply = createAck(ack_hash);
        if (reply) {
          reply->payload[reply->payload_len++] = getUnsyncedCount(slot, client);
          sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
        }
      }
    } else {
      int reply_len = handleRequest(slot, client, sender_timestamp, &data[4], len - 4);
      if (reply_len > 0) {
        if (packet->isRouteFlood()) {
          mesh::Packet* path = createPathReturn(client->id, secret,
                                                 packet->path, packet->path_len,
                                                 PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
          if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        } else {
          mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
          if (reply) {
            if (client->out_path_len != OUT_PATH_UNKNOWN) {
              sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
            } else {
              sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
            }
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  onPeerPathRecv                                                      */
/* ------------------------------------------------------------------ */
bool MultiRoomMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx,
                                   const uint8_t* secret,
                                   uint8_t* path, uint8_t path_len,
                                   uint8_t extra_type, uint8_t* extra,
                                   uint8_t extra_len) {
  RoomSlot& slot = rooms[_active_slot];
  int i = matching_peer_indexes[sender_idx];
  if (i >= 0 && i < slot.acl.getNumClients()) {
    ClientInfo* client = slot.acl.getClientByIdx(i);
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  }

  if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) {
    processAckForSlot(slot, extra);
  }
  return false;
}

/* ------------------------------------------------------------------ */
/*  onAckRecv — check all rooms                                         */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::onAckRecv(mesh::Packet* packet, uint32_t ack_crc) {
  for (int s = 0; s < MAX_ROOMS; s++) {
    if (!rooms[s].active) continue;
    if (processAckForSlot(rooms[s], (uint8_t*)&ack_crc)) {
      packet->markDoNotRetransmit();
      return;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Per-slot helpers                                                    */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::addPost(RoomSlot& slot, ClientInfo* client, const char* text, uint32_t msg_id) {
  uint8_t ridx = (uint8_t)(&slot - rooms);
  if (ridx == 0) return;  // JES-846: room 0 is identity-only, no posts allowed

  // JES-861 echo suppression: if we already hold this exact logical message
  // (same author + same sender message-id), don't create a duplicate. This
  // covers the case where the peer's replicated copy arrived via sync before
  // our own direct reception of the flooded message.
  if (msg_id != 0) {
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (_post_pool[i].room_idx == ridx &&
          _post_pool[i].msg_id == msg_id &&
          memcmp(_post_pool[i].author.pub_key, client->id.pub_key, 4) == 0)
        return;   // already have this message
    }
  }

  int quota = MAX_TOTAL_POSTS / (_num_active_rooms > 0 ? _num_active_rooms : 1);

  // Find a free slot; also track oldest post owned by this room
  PostInfo* free_slot = nullptr;
  PostInfo* oldest_for_room = nullptr;
  int room_count = 0;

  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    PostInfo& p = _post_pool[i];
    if (p.room_idx == 0xFF) {
      if (!free_slot) free_slot = &p;
    } else if (p.room_idx == ridx) {
      room_count++;
      if (!oldest_for_room || p.post_timestamp < oldest_for_room->post_timestamp)
        oldest_for_room = &p;
    }
  }

  // If this room is at quota, evict its oldest post to make room
  if (room_count >= quota && oldest_for_room) {
    memset(oldest_for_room, 0, sizeof(PostInfo));
    oldest_for_room->room_idx = 0xFF;
    if (!free_slot) free_slot = oldest_for_room;
  }

  // Last resort: pool completely full — evict the globally oldest post
  if (!free_slot) {
    PostInfo* oldest_global = nullptr;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (!oldest_global || _post_pool[i].post_timestamp < oldest_global->post_timestamp)
        oldest_global = &_post_pool[i];
    }
    memset(oldest_global, 0, sizeof(PostInfo));
    oldest_global->room_idx = 0xFF;
    free_slot = oldest_global;
  }

  free_slot->author         = client->id;
  StrHelper::strncpy(free_slot->text, text, MAX_POST_TEXT_LEN);
  free_slot->post_timestamp = getRTCClock()->getCurrentTimeUnique();
  free_slot->msg_id         = msg_id;   // JES-861: cross-node message identity
  free_slot->room_idx       = ridx;
  // Phase 5: tag with this room-server as origin.
  // JES-874: origin MUST be the NODE identity (rooms[0]), not the room key.
  // Coupled nodes share the same room key, so using slot.id here made both
  // nodes stamp posts with an identical origin_id. The anti-entropy VV is a
  // per-origin high-watermark, so a node whose watermark had advanced could
  // never pull older posts the peer authored under that same shared origin
  // (e.g. operator/[OP] posts). Using the per-node identity makes each
  // room-server a distinct origin, which is what the VV model requires.
  memcpy(free_slot->origin_id, rooms[0].id.pub_key, 4);

  DLOG(free_slot->post_timestamp,
       "RX post room=%d auth=%02x%02x%02x%02x ts=%lu txt=%.30s",
       (int)ridx,
       (unsigned)free_slot->author.pub_key[0], (unsigned)free_slot->author.pub_key[1],
       (unsigned)free_slot->author.pub_key[2], (unsigned)free_slot->author.pub_key[3],
       (unsigned long)free_slot->post_timestamp, text);

  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  slot.num_posted++;
  _post_dirty_at = futureMillis(5000);  // debounced persist (JES-794)

  // Per-client message counter (JES-800)
  if (client && client->msg_count < 0xFFFF) client->msg_count++;

  // Histogram ring-buffer: advance to current bucket then increment (JES-800)
  histAdvance(free_slot->post_timestamp);
  _hist_ring[_hist_head]++;

  // Phase 5: update local VV and push to peers immediately
  vvUpdate(slot, free_slot->origin_id, free_slot->post_timestamp);
  for (int pi = 0; pi < MAX_PEERS; pi++) {
    if (peers[pi].active) pushPostToPeer(pi, slot, *free_slot);
  }

  // Notify MQTT transport (JES-792 Phase a) — publish encrypted envelope
  if (_mqtt_post_cb) {
    _mqtt_post_cb((int)ridx, free_slot->post_timestamp,
                  free_slot->author.pub_key, free_slot->text,
                  _mqtt_post_ctx);
  }
}

/* Advance the histogram ring-buffer so _hist_ring[_hist_head] covers now_ts.
 * Skipped buckets (node was offline or no posts for >1 h) are zeroed. */
void MultiRoomMesh::histAdvance(uint32_t now_ts) {
  if (_hist_bucket_ts == 0) {
    // First ever post — seed bucket timestamp
    _hist_bucket_ts = now_ts;
    return;
  }
  if (now_ts < _hist_bucket_ts) return;  // clock skew guard
  uint32_t elapsed_secs = now_ts - _hist_bucket_ts;
  uint32_t buckets_elapsed = elapsed_secs / 3600u;
  if (buckets_elapsed == 0) return;  // still within current bucket

  // Advance at most HIST_BUCKETS steps; if more, zero the entire ring
  uint32_t steps = buckets_elapsed < (uint32_t)HIST_BUCKETS
                   ? buckets_elapsed : (uint32_t)HIST_BUCKETS;
  for (uint32_t s = 0; s < steps; s++) {
    _hist_head = (_hist_head + 1) % HIST_BUCKETS;
    _hist_ring[_hist_head] = 0;
  }
  _hist_bucket_ts += buckets_elapsed * 3600u;  // align to bucket boundary
}

void MultiRoomMesh::pushPostToClient(RoomSlot& slot, ClientInfo* client, PostInfo& post) {
  DLOG(post.post_timestamp,
       "TX push→client room=%d client=%02x%02x%02x%02x ts=%lu",
       (int)(uint8_t)(&slot - rooms),
       (unsigned)client->id.pub_key[0], (unsigned)client->id.pub_key[1],
       (unsigned)client->id.pub_key[2], (unsigned)client->id.pub_key[3],
       (unsigned long)post.post_timestamp);
  int len = 0;
  memcpy(&reply_data[len], &post.post_timestamp, 4); len += 4;

  uint8_t attempt;
  getRNG()->random(&attempt, 1);
  reply_data[len++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3);

  memcpy(&reply_data[len], post.author.pub_key, 4); len += 4;

  int text_len = strlen(post.text);
  memcpy(&reply_data[len], post.text, text_len); len += text_len;

  mesh::Utils::sha256((uint8_t*)&client->extra.room.pending_ack, 4,
                       reply_data, len, client->id.pub_key, PUB_KEY_SIZE);
  client->extra.room.push_post_timestamp = post.post_timestamp;

  // Temporarily set self_id to this room for createDatagram signing
  self_id = slot.id;
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, reply_data, len);
  if (pkt) {
    if (client->out_path_len == OUT_PATH_UNKNOWN) {
      sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
      client->extra.room.ack_timeout = futureMillis(PUSH_ACK_TIMEOUT_FLOOD);
    } else {
      sendDirect(pkt, client->out_path, client->out_path_len);
      uint8_t hops = client->out_path_len & 63;
      client->extra.room.ack_timeout = futureMillis(PUSH_TIMEOUT_BASE + PUSH_ACK_TIMEOUT_FACTOR * (hops + 1));
    }
    slot.num_post_pushes++;
  } else {
    client->extra.room.pending_ack = 0;
  }
}

uint8_t MultiRoomMesh::getUnsyncedCount(RoomSlot& slot, ClientInfo* client) {
  uint8_t ridx = (uint8_t)(&slot - rooms);
  uint8_t count = 0;
  for (int k = 0; k < MAX_TOTAL_POSTS; k++) {
    const PostInfo& p = _post_pool[k];
    if (p.room_idx == ridx &&
        p.post_timestamp > client->extra.room.sync_since &&
        !p.author.matches(client->id)) {
      count++;
    }
  }
  return count;
}

bool MultiRoomMesh::processAckForSlot(RoomSlot& slot, const uint8_t* data) {
  for (int i = 0; i < slot.acl.getNumClients(); i++) {
    auto c = slot.acl.getClientByIdx(i);
    if (c->extra.room.pending_ack &&
        memcmp(data, &c->extra.room.pending_ack, 4) == 0) {
      c->extra.room.pending_ack  = 0;
      c->extra.room.push_failures = 0;
      c->extra.room.sync_since   = c->extra.room.push_post_timestamp;
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------ */
/*  Advertisement helpers                                               */
/* ------------------------------------------------------------------ */
mesh::Packet* MultiRoomMesh::createRoomAdvert(RoomSlot& slot) {
  // Build advert data using the canonical MeshCore wire format (AdvertDataBuilder).
  // The old hand-rolled layout never set ADV_NAME_MASK, so remote parsers
  // (companion app) saw no name and displayed only the pubkey (JES-868). It also
  // encoded lat/lon as raw floats after the name instead of int32 (*1E6) before
  // it, so locations never decoded either. Delegating to AdvertDataBuilder fixes
  // both by setting the correct flags/offsets.
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len;

  if (slot.lat != 0.0f || slot.lon != 0.0f) {
    AdvertDataBuilder builder(ADV_TYPE_ROOM, slot.name, slot.lat, slot.lon);
    app_data_len = builder.encodeTo(app_data);
  } else {
    AdvertDataBuilder builder(ADV_TYPE_ROOM, slot.name);
    app_data_len = builder.encodeTo(app_data);
  }

  // Swap self_id to this room before creating the advert (which signs with priv_key)
  self_id = slot.id;
  return createAdvert(slot.id, app_data, app_data_len);
}

void MultiRoomMesh::sendRoomAdvertisement(RoomSlot& slot, int delay_millis, bool flood) {
  mesh::Packet* pkt = createRoomAdvert(slot);
  if (!pkt) return;
  if (flood) {
    sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
  } else {
    sendZeroHop(pkt, delay_millis);
  }
}

void MultiRoomMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (rooms[i].stealth) continue;  // stealth rooms never advertise
    sendRoomAdvertisement(rooms[i], delay_millis + (uint32_t)i * 1000, flood);
  }
}

/* Web-triggered manual flood advert (JES-868). Runs on the AsyncTCP task, so it
 * MUST NOT touch the radio/self_id here (would race the mesh loop, cf JES-864).
 * Only set the pending flag; loop() performs the actual TX on the mesh task. */
void MultiRoomMesh::triggerAdvertFromWeb() {
  _web_advert_pending = true;
}

/* Record metadata of a received packet into the RX live-view ring (JES-868).
 * Called from onRecvPacket() on the mesh task (single writer). No payload is
 * stored — traffic may be encrypted for other nodes (privacy). */
void MultiRoomMesh::recordRxLog(mesh::Packet* pkt) {
  if (!pkt) return;
  uint8_t h = _rxlog_head;
  RxLogEntry& e = _rxlog[h];
  e.rx_millis = millis();
  e.rtc       = getRTCClock()->getCurrentTime();
  e.rssi      = (int16_t)radio_driver.getLastRSSI();
  e.snr       = (int8_t)radio_driver.getLastSNR();
  e.ptype     = pkt->getPayloadType();
  e.route     = pkt->isRouteFlood() ? 0 : 1;
  e.dhash     = (pkt->payload_len >= 1) ? pkt->payload[0] : 0;
  e.path_len  = pkt->getPathHashCount();
  e.plen      = (uint8_t)(pkt->payload_len > 255 ? 255 : pkt->payload_len);
  _rxlog_head = (uint8_t)((h + 1) % RX_LOG_SIZE);
  _rxlog_total++;
}

/* ------------------------------------------------------------------ */
/*  Advert timer management                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::updateAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (rooms[i].stealth) {
      rooms[i].next_local_advert = 0;  // stealth: disable
    } else if (_advert_interval_sec > 0) {
      rooms[i].next_local_advert = futureMillis((uint32_t)_advert_interval_sec * 1000);
    } else {
      rooms[i].next_local_advert = 0;
    }
  }
}

void MultiRoomMesh::updateFloodAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (rooms[i].stealth) {
      rooms[i].next_flood_advert = 0;  // stealth: disable
    } else if (_prefs.flood_advert_interval > 0) {
      rooms[i].next_flood_advert = futureMillis((uint32_t)_prefs.flood_advert_interval * 60 * 60 * 1000);
    } else {
      rooms[i].next_flood_advert = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  handleRequest (REQ_TYPE_*)                                          */
/* ------------------------------------------------------------------ */
struct ServerStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;
  uint16_t n_direct_dups, n_flood_dups;
  uint16_t n_posted, n_post_push;
};

int MultiRoomMesh::handleRequest(RoomSlot& slot, ClientInfo* sender,
                                  uint32_t sender_timestamp,
                                  uint8_t* payload, size_t payload_len) {
  memcpy(reply_data, &sender_timestamp, 4);

  if (payload[0] == REQ_TYPE_GET_STATUS) {
    ServerStats stats;
    stats.batt_milli_volts    = board.getBattMilliVolts();
    stats.curr_tx_queue_len   = _mgr->getOutboundTotal();
    stats.noise_floor         = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi           = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv      = radio_driver.getPacketsRecv();
    stats.n_packets_sent      = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs  = uptime_millis / 1000;
    stats.n_sent_flood        = getNumSentFlood();
    stats.n_sent_direct       = getNumSentDirect();
    stats.n_recv_flood        = getNumRecvFlood();
    stats.n_recv_direct       = getNumRecvDirect();
    stats.err_events          = _err_flags;
    stats.last_snr            = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups       = ((SimpleMeshTables*)getTables())->getNumDirectDups();
    stats.n_flood_dups        = ((SimpleMeshTables*)getTables())->getNumFloodDups();
    stats.n_posted            = slot.num_posted;
    stats.n_post_push         = slot.num_post_pushes;
    memcpy(&reply_data[4], &stats, sizeof(stats));
    return 4 + sizeof(stats);
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]);
    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) perm_mask = 0x00;
    sensors.querySensors(perm_mask, telemetry);
    float temperature = board.getMCUTemperature();
    if (!isnan(temperature)) telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature);
    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen;
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    if (payload[1] == 0 && payload[2] == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < slot.acl.getNumClients() && ofs + 7 <= (int)sizeof(reply_data) - 4; i++) {
        auto c = slot.acl.getClientByIdx(i);
        if (!c->isAdmin()) continue;
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  loopSlot — post-push logic per room                                 */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::loopSlot(RoomSlot& slot) {
  if (!millisHasNowPassed(slot.next_push)) return;
  if (slot.acl.getNumClients() == 0) {
    slot.next_push = futureMillis(SYNC_PUSH_INTERVAL);
    return;
  }

  // Check ACK timeouts
  for (int i = 0; i < slot.acl.getNumClients(); i++) {
    auto c = slot.acl.getClientByIdx(i);
    if (c->extra.room.pending_ack && millisHasNowPassed(c->extra.room.ack_timeout)) {
      c->extra.room.push_failures++;
      c->extra.room.pending_ack = 0;
    }
  }

  // Round-robin: try to push next unsynced post to next client
  auto client = slot.acl.getClientByIdx(slot.next_client_idx);
  bool did_push = false;

  if (client->extra.room.pending_ack == 0 &&
      client->last_activity != 0 &&
      client->extra.room.push_failures < 3) {

    uint8_t ridx = (uint8_t)(&slot - rooms);
    uint32_t now = getRTCClock()->getCurrentTime();
    PostInfo* oldest_unsynced = nullptr;
    for (int k = 0; k < MAX_TOTAL_POSTS; k++) {
      PostInfo& p = _post_pool[k];
      if (p.room_idx == ridx &&
          now >= p.post_timestamp + POST_SYNC_DELAY_SECS &&
          p.post_timestamp > client->extra.room.sync_since &&
          !p.author.matches(client->id)) {
        if (!oldest_unsynced || p.post_timestamp < oldest_unsynced->post_timestamp)
          oldest_unsynced = &p;
      }
    }
    if (oldest_unsynced) {
      self_id = slot.id;
      pushPostToClient(slot, client, *oldest_unsynced);
      did_push = true;
    }
  }

  slot.next_client_idx = (slot.next_client_idx + 1) % slot.acl.getNumClients();
  slot.next_push = futureMillis(did_push ? SYNC_PUSH_INTERVAL : SYNC_PUSH_INTERVAL / 8);

  // Lazy contacts save
  if (slot.dirty_contacts_expiry && millisHasNowPassed(slot.dirty_contacts_expiry)) {
    // ACL persistence not implemented in Phase 1 (in-memory only)
    slot.dirty_contacts_expiry = 0;
  }
}

/* ------------------------------------------------------------------ */
/*  main loop                                                           */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::loop() {
  mesh::Mesh::loop();

  // Push posts and run advert timers per room
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    RoomSlot& slot = rooms[i];

    loopSlot(slot);

    // Advert timers — stealth rooms never advertise; timers stay 0 (never fire)
    if (!slot.stealth) {
      if (slot.next_flood_advert && millisHasNowPassed(slot.next_flood_advert)) {
        self_id = slot.id;
        sendRoomAdvertisement(slot, 0, true);
        // Reschedule using the configurable flood interval (hours); 0 = disable.
        if (_prefs.flood_advert_interval > 0) {
          slot.next_flood_advert = futureMillis(
              (uint32_t)_prefs.flood_advert_interval * 60UL * 60UL * 1000UL + (uint32_t)i * 15000);
        } else {
          slot.next_flood_advert = 0;
        }
        slot.next_local_advert = futureMillis((uint32_t)_advert_interval_sec * 1000 + (uint32_t)i * 15000);
      } else if (slot.next_local_advert && millisHasNowPassed(slot.next_local_advert)) {
        self_id = slot.id;
        sendRoomAdvertisement(slot, 0, false);
        slot.next_local_advert = futureMillis((uint32_t)_advert_interval_sec * 1000 + (uint32_t)i * 15000);
      }
    }
  }

  // Temporary radio params
  if (set_radio_at && millisHasNowPassed(set_radio_at)) {
    set_radio_at = 0;
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
  }
  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) {
    revert_radio_at = 0;
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  }

  // Phase 5: periodic anti-entropy — SYNCREQ to each configured peer
  for (int pi = 0; pi < MAX_PEERS; pi++) {
    if (!peers[pi].active) continue;
    if (peers[pi].next_sync_at && millisHasNowPassed(peers[pi].next_sync_at)) {
      sendSyncReq(pi);
      peers[pi].next_sync_at = futureMillis((uint32_t)_sync_interval_s * 1000);
    }
    // Periodic room-key re-sync (JES-848): propagates rooms created while peer was offline
    // Receive-side dedup (handleRoomSync) makes repeated sends safe.
    if (peers[pi].next_roomsync_at && millisHasNowPassed(peers[pi].next_roomsync_at)) {
      sendRoomSync(pi);
      peers[pi].next_roomsync_at = futureMillis(PEER_ROOMSYNC_INTERVAL_MS);
    }
  }

  // Deferred web-triggered sync (JES-864): executed here on the mesh task so the
  // radio TX + self_id swap never race the AsyncTCP web callback (Core0).
  if (_web_syncreq_pending) {
    _web_syncreq_pending = false;
    int idx = _web_syncreq_idx;
    if (idx >= 0) {
      if (idx < MAX_PEERS && peers[idx].active) sendSyncReq(idx);
    } else {
      for (int i = 0; i < MAX_PEERS; i++) if (peers[i].active) sendSyncReq(i);
    }
  }
  if (_web_fullsync_pending) {
    _web_fullsync_pending = false;
    int idx = _web_fullsync_idx;
    if (idx >= 0) {
      if (idx < MAX_PEERS && peers[idx].active) sendSyncReq(idx, true);
    } else {
      for (int i = 0; i < MAX_PEERS; i++) if (peers[i].active) sendSyncReq(i, true);
    }
  }
  if (_web_roomsync_pending) {
    _web_roomsync_pending = false;
    int idx = _web_roomsync_idx;
    if (idx >= 0) {
      if (idx < MAX_PEERS && peers[idx].active) sendRoomSync(idx);
    } else {
      for (int i = 0; i < MAX_PEERS; i++) if (peers[i].active) sendRoomSync(i);
    }
  }

  // Deferred web-triggered flood advert (JES-868): performed on the mesh task so
  // the radio TX + self_id swap never race the AsyncTCP web callback (JES-864).
  if (_web_advert_pending) {
    _web_advert_pending = false;
    sendSelfAdvertisement(0, true);   // flood; skips stealth rooms internally
  }

  // Deferred web-triggered neighbour discovery (JES-869): zero-hop TX runs on the
  // mesh task so it never races the AsyncTCP web callback (cf JES-864).
  if (_web_discover_pending) {
    _web_discover_pending = false;
    sendNodeDiscoverReq();
  }

  // Debounced post-pool persistence (JES-794): write SPIFFS ~5s after last new post
  if (_post_dirty_at && millisHasNowPassed(_post_dirty_at)) {
    _post_dirty_at = 0;
    savePostPool();
  }

  // Uptime tracking
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;

  // Keep self_id at room[0] when idle
  self_id = rooms[0].id;
}

/* ------------------------------------------------------------------ */
/*  handleCommand — room management CLI + shared CLI passthrough        */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::handleCommand(uint32_t sender_timestamp,
                                   char* command, char* reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {
      region_map = temp_map;
      region_load_active = false;
      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char* np = command;
      while (*np == ' ') np++;
      int indent = np - command;
      char* ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }
      while (*ep && *ep != 'F') ep++;
      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw  = temp_map.putRegion(np, parent->id, old ? old->id : 0);
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);
            load_stack[indent] = nw;
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++;

  // Optional companion-radio prefix
  if (strlen(command) > 4 && command[2] == '|') {
    memcpy(reply, command, 3);
    reply += 3;
    command += 3;
  }

  // serial=true when called from the local serial CLI (sender_timestamp==0)
  bool is_serial = (sender_timestamp == 0);

  // ---- room management commands ----
  if (memcmp(command, "room ", 5) == 0) {
    handleRoomCommand(command + 5, reply, is_serial);
    return;
  }

  // ---- ACL commands scoped to active_slot (or room[0] for serial) ----
  int scope = is_serial ? 0 : _active_slot;
  if (memcmp(command, "setperm ", 8) == 0) {
    char* hex = &command[8];
    char* sp  = strchr(hex, ' ');
    if (!sp) { strcpy(reply, "Err - bad params"); return; }
    *sp++ = 0;
    uint8_t pubkey[PUB_KEY_SIZE];
    int hex_len = min((int)(sp - hex), PUB_KEY_SIZE * 2);
    if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
      uint8_t perms = atoi(sp);
      if (rooms[scope].acl.applyPermissions(rooms[scope].id, pubkey, hex_len / 2, perms)) {
        rooms[scope].dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
        strcpy(reply, "OK");
      } else {
        strcpy(reply, "Err - invalid params");
      }
    } else {
      strcpy(reply, "Err - bad pubkey");
    }
    return;
  }
  if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.printf("ACL room[%d]:\n", scope);
    for (int i = 0; i < rooms[scope].acl.getNumClients(); i++) {
      auto c = rooms[scope].acl.getClientByIdx(i);
      if (c->permissions == 0) continue;
      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.println();
    }
    reply[0] = 0;
    return;
  }

  // ---- stealth on|off|status — global (all rooms) visibility control ----
  if (memcmp(command, "stealth", 7) == 0) {
    const char* arg = command + 7;
    while (*arg == ' ') arg++;
    if (strcmp(arg, "on") == 0) {
      setRoomStealth(-1, true);   // all rooms
      strcpy(reply, "OK - stealth ON (all rooms hidden, no adverts)");
    } else if (strcmp(arg, "off") == 0) {
      setRoomStealth(-1, false);  // all rooms
      strcpy(reply, "OK - stealth OFF (all rooms visible, adverts enabled)");
    } else {
      // "stealth status" or just "stealth"
      bool any_visible = false;
      for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && !rooms[i].stealth) { any_visible = true; break; }
      }
      sprintf(reply, "stealth=%s (%d active rooms)",
              any_visible ? "partial/off" : "on",
              _num_active_rooms);
    }
    return;
  }

  // ---- repeater on|off|status — friendly alias for set fwd (JES-855) ----
  // Independent of stealth: stealth = room advertising; repeater = flood forwarding.
  if (memcmp(command, "repeater", 8) == 0 && (command[8] == ' ' || command[8] == 0)) {
    const char* arg = command + 8;
    while (*arg == ' ') arg++;
    if (strcmp(arg, "on") == 0) {
      char cmd[20] = "set repeat on";
      handleCommand(_active_slot, cmd, reply);
    } else if (strcmp(arg, "off") == 0) {
      char cmd[20] = "set repeat off";
      handleCommand(_active_slot, cmd, reply);
    } else {
      // "repeater status" or just "repeater"
      sprintf(reply, "repeater=%s (stealth is separate)",
              _prefs.disable_fwd ? "off" : "on");
    }
    return;
  }

  // ---- set txpower <n> — applies live and persists ----
  if (memcmp(command, "set txpower ", 12) == 0) {
    int8_t pwr = (int8_t)atoi(command + 12);
    if (pwr < 2 || pwr > 22) {
      strcpy(reply, "Err: txpower must be 2-22 dBm");
    } else {
      _prefs.tx_power_dbm = pwr;
      _cli.savePrefs(_fs);
      setTxPower(pwr);
      sprintf(reply, "OK - txpower %d dBm applied", pwr);
    }
    return;
  }

  // ---- repeater / global-settings guard: management room only ----
  // Commands that touch global node-prefs (repeat, flood limits, region)
  // must only be issued from room[0] (management room) or serial CLI.
  if (sender_timestamp != 0 && _active_slot != 0) {
    if (memcmp(command, "set repeat", 10) == 0 ||
        memcmp(command, "set flood.max", 13) == 0 ||
        memcmp(command, "region ", 7) == 0 ||
        memcmp(command, "repeater ", 9) == 0) {
      strcpy(reply, "Err - repeater settings only available on management room");
      return;
    }
  }

  // ---- debug log toggle (JES-852) ----
  if (memcmp(command, "debug ", 6) == 0) {
    const char* arg = command + 6;
    while (*arg == ' ') arg++;
    if (strcmp(arg, "on") == 0) {
      g_dbglog.enable(true);
      strcpy(reply, "OK - debug logging ON");
    } else if (strcmp(arg, "off") == 0) {
      g_dbglog.enable(false);
      strcpy(reply, "OK - debug logging OFF");
    } else if (strcmp(arg, "clear") == 0) {
      g_dbglog.clear();
      strcpy(reply, "OK - debug log cleared");
    } else if (strcmp(arg, "status") == 0) {
      snprintf(reply, 160, "debug log: %s  entries=%d/%d",
               g_dbglog.isEnabled() ? "ON" : "OFF",
               (int)g_dbglog.count(), (int)DEBUG_LOG_MAX_ENTRIES);
    } else {
      strcpy(reply, "debug on|off|clear|status");
    }
    return;
  }

  // ---- peer management commands (Phase 5 ground work) ----
  if (memcmp(command, "peer", 4) == 0 && (command[4] == ' ' || command[4] == 0)) {
    handlePeerCommand(command + 4, reply, is_serial);
    return;
  }

  // ---- login notification target management (JES-834) ----
  // notify <room_idx> list|add <hex64>|del <hex64>
  if (memcmp(command, "notify ", 7) == 0) {
    char* args = command + 7;
    while (*args == ' ') args++;
    int room_idx = atoi(args);
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    // args now points to sub-command: list|add|del
    if (room_idx < 0 || room_idx >= MAX_ROOMS || !isRoomActive(room_idx)) {
      strcpy(reply, "Err - invalid or inactive room index"); return;
    }
    if (strcmp(args, "list") == 0) {
      int cnt = getNotifyTargetCount(room_idx);
      if (cnt == 0) {
        snprintf(reply, 160, "notify room[%d]: no targets configured", room_idx);
      } else {
        int pos = snprintf(reply, 160, "notify room[%d] (%d):", room_idx, cnt);
        for (int i = 0; i < cnt && pos < 155; i++) {
          const uint8_t* k = getNotifyTarget(room_idx, i);
          char hex[9];
          snprintf(hex, sizeof(hex), " %02x%02x%02x%02x", k[0], k[1], k[2], k[3]);
          pos += snprintf(reply + pos, 160 - pos, "%s", hex);
        }
      }
      return;
    }
    if (memcmp(args, "add ", 4) == 0 || memcmp(args, "del ", 4) == 0) {
      bool do_add = (args[0] == 'a');
      char* hex = args + 4;
      while (*hex == ' ') hex++;
      int hexlen = strlen(hex);
      if (hexlen < PUB_KEY_SIZE * 2) {
        strcpy(reply, "Err - pubkey must be 64 hex chars"); return;
      }
      uint8_t pub_key[PUB_KEY_SIZE];
      if (!mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, hex)) {
        strcpy(reply, "Err - invalid hex pubkey"); return;
      }
      if (do_add) {
        if (addNotifyTarget(room_idx, pub_key)) {
          snprintf(reply, 160, "OK - notify target added to room[%d]", room_idx);
        } else {
          snprintf(reply, 160, "Err - target list full (max %d)", MAX_NOTIFY_TARGETS);
        }
      } else {
        if (delNotifyTarget(room_idx, pub_key)) {
          snprintf(reply, 160, "OK - notify target removed from room[%d]", room_idx);
        } else {
          strcpy(reply, "Err - target not found");
        }
      }
      return;
    }
    strcpy(reply, "Err - usage: notify <room_idx> list|add <hex64>|del <hex64>");
    return;
  }

  // ---- sync interval [<seconds>] — configurable anti-entropy pull period (JES-844) ----
  if (memcmp(command, "sync interval", 13) == 0) {
    const char* arg = command + 13;
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
      sprintf(reply, "sync interval=%lu s (range 10-3600)", (unsigned long)_sync_interval_s);
    } else {
      int sec = atoi(arg);
      if (sec < 10 || sec > 3600) {
        strcpy(reply, "Err: interval must be 10-3600 seconds");
      } else {
        setSyncIntervalSec((uint32_t)sec);
        sprintf(reply, "OK - sync interval %lu s", (unsigned long)_sync_interval_s);
      }
    }
    return;
  }

  // ---- advert interval [<seconds>] — global local advert period ----
  if (memcmp(command, "advert interval", 15) == 0) {
    const char* arg = command + 15;
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
      sprintf(reply, "advert interval=%d s (range 10-3600)", (int)_advert_interval_sec);
    } else {
      int sec = atoi(arg);
      if (sec < 10 || sec > 3600) {
        strcpy(reply, "Err: interval must be 10-3600 seconds");
      } else {
        setAdvertIntervalSec((uint16_t)sec);
        sprintf(reply, "OK - advert interval %d s", (int)_advert_interval_sec);
      }
    }
    return;
  }

  // ---- rooms — list active rooms ----
  if (strcmp(command, "rooms") == 0) {
    if (is_serial) {
      Serial.printf("Active rooms (%d):\n", _num_active_rooms);
      for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].active) continue;
        Serial.printf("  [%d] %-23s  clients=%d  posts=%d  stealth=%s\n",
                      i, rooms[i].name,
                      rooms[i].acl.getNumClients(),
                      (int)rooms[i].num_posted,
                      rooms[i].stealth ? "on" : "off");
      }
      reply[0] = 0;
    } else {
      char* p = reply;
      char* end = reply + 180;
      for (int i = 0; i < MAX_ROOMS && p < end - 20; i++) {
        if (!rooms[i].active) continue;
        p += snprintf(p, end - p, "[%d]%.16s(%dc) ", i, rooms[i].name,
                      rooms[i].acl.getNumClients());
      }
      if (p == reply) strcpy(reply, "no active rooms");
    }
    return;
  }

  // ---- msgs <idx> [n] — show last n posts from a room ----
  if (memcmp(command, "msgs", 4) == 0 && (command[4] == ' ' || command[4] == 0)) {
    const char* arg = command + 4;
    while (*arg == ' ') arg++;
    int ridx = atoi(arg);
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    int n = (*arg) ? atoi(arg) : 10;
    if (n < 1 || n > 50) n = 10;

    if (ridx < 0 || ridx >= MAX_ROOMS || !rooms[ridx].active) {
      strcpy(reply, "Err: invalid room idx"); return;
    }

    // Collect posts for this room sorted by timestamp ascending
    // Simple selection sort over the pool (small pool, non-hot path)
    const PostInfo* sorted[MAX_TOTAL_POSTS];
    int cnt = 0;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (_post_pool[i].room_idx == (uint8_t)ridx) sorted[cnt++] = &_post_pool[i];
    }
    // Insertion sort by timestamp
    for (int i = 1; i < cnt; i++) {
      const PostInfo* key = sorted[i];
      int j = i - 1;
      while (j >= 0 && sorted[j]->post_timestamp > key->post_timestamp) {
        sorted[j + 1] = sorted[j]; j--;
      }
      sorted[j + 1] = key;
    }

    int start = (cnt > n) ? cnt - n : 0;
    if (is_serial) {
      Serial.printf("Room [%d] '%s' — %d/%d posts shown:\n", ridx, rooms[ridx].name, cnt - start, cnt);
      for (int i = start; i < cnt; i++) {
        const PostInfo* p = sorted[i];
        Serial.printf("  [%u] <%s> %s\n",
                      (unsigned)p->post_timestamp,
                      resolveName(p->author.pub_key),
                      p->text);
      }
      reply[0] = 0;
    } else {
      // Mesh CLI: show last 1-2 posts compact
      char* p = reply;
      char* end = reply + 180;
      p += snprintf(p, end - p, "%d posts:", cnt);
      int show = (cnt > 2) ? 2 : cnt;
      for (int i = cnt - show; i < cnt && p < end - 10; i++) {
        p += snprintf(p, end - p, " <%s>%.30s",
                      resolveName(sorted[i]->author.pub_key), sorted[i]->text);
      }
    }
    return;
  }

  // ---- nicks <idx> — show nicklist for a room ----
  if (memcmp(command, "nicks", 5) == 0 && (command[5] == ' ' || command[5] == 0)) {
    const char* arg = command + 5;
    while (*arg == ' ') arg++;
    int ridx = (*arg) ? atoi(arg) : 0;

    if (ridx < 0 || ridx >= MAX_ROOMS || !rooms[ridx].active) {
      strcpy(reply, "Err: invalid room idx"); return;
    }

    static const char* ROLE_NAMES[] = { "guest", "ro", "rw", "admin" };
    if (is_serial) {
      int nc = rooms[ridx].acl.getNumClients();
      Serial.printf("Room [%d] '%s' — %d clients:\n", ridx, rooms[ridx].name, nc);
      for (int i = 0; i < nc; i++) {
        ClientInfo* ci = rooms[ridx].acl.getClientByIdx(i);
        if (ci->permissions == 0) continue;
        uint8_t role = ci->permissions & PERM_ACL_ROLE_MASK;
        Serial.printf("  %-8s [%s]  last=%u\n",
                      resolveName(ci->id.pub_key),
                      ROLE_NAMES[role < 4 ? role : 0],
                      (unsigned)ci->last_activity);
      }
      reply[0] = 0;
    } else {
      char* p = reply;
      char* end = reply + 180;
      int nc = rooms[ridx].acl.getNumClients();
      p += snprintf(p, end - p, "r%d(%d):", ridx, nc);
      for (int i = 0; i < nc && p < end - 12; i++) {
        ClientInfo* ci = rooms[ridx].acl.getClientByIdx(i);
        if (ci->permissions == 0) continue;
        uint8_t role = ci->permissions & PERM_ACL_ROLE_MASK;
        p += snprintf(p, end - p, " %s/%s", resolveName(ci->id.pub_key),
                      ROLE_NAMES[role < 4 ? role : 0]);
      }
    }
    return;
  }

  // ---- say <idx> <text> — server-authored post ----
  if (memcmp(command, "say ", 4) == 0) {
    const char* arg = command + 4;
    while (*arg == ' ') arg++;
    int ridx = atoi(arg);
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    if (ridx < 0 || ridx >= MAX_ROOMS || !rooms[ridx].active || ridx == 0) {
      strcpy(reply, "Err: invalid room idx (use 1+, room 0 is identity-only)"); return;
    }
    if (*arg == 0) { strcpy(reply, "Err: missing text"); return; }
    addServerPost(ridx, arg);
    snprintf(reply, 60, "OK - posted to room %d", ridx);
    return;
  }

  // ---- help / ? — grouped CLI reference ----
  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
    if (is_serial) {
      Serial.println();
      Serial.println("+---------------------------------------------------------------+");
      Serial.println("|                      SIREN CLI Help                          |");
      Serial.println("+---------------------------------------------------------------+");
      Serial.println();
      Serial.println("  ROOMS");
      Serial.println("  -----");
      Serial.println("  rooms                          List active rooms (compact)");
      Serial.println("  room list                      List all rooms with details");
      Serial.println("  room add                       Add a new room slot");
      Serial.println("  room del <idx>                 Delete room [serial only]");
      Serial.println("  room delpost <idx> <hex8> <ts> Delete post by origin_id+ts [serial only, y/N confirm]");
      Serial.println("  room export <idx>              Print private key as 128 hex chars [serial only]");
      Serial.println("  room import <idx> <128hex>     Import 64-byte private key into slot [serial only]");
      Serial.println("  room rekey <idx>               Show rekey warning [serial only]");
      Serial.println("  room rekey <idx> confirm       Rotate private key (2-step) [serial only]");
      Serial.println("  room export <idx>              Print private key hex [serial only]");
      Serial.println("  room import <idx> <hex128>     Import private key from another node [serial only]");
      Serial.println("  room set <idx> name <val>      Set room name");
      Serial.println("  room set <idx> pass <val>      Show password-change warning (step 1)");
      Serial.println("  room set <idx> pass <val> confirm  Set room password (2-step confirm)");
      Serial.println("  room set <idx> guest <val>     Set guest access password");
      Serial.println("  room set <idx> lat <val>       Set advertised latitude (-90..90, 0=uit)");
      Serial.println("  room set <idx> lon <val>       Set advertised longitude (-180..180, 0=uit)");
      Serial.println("  room stealth <idx> on|off      Toggle room visibility");
      Serial.println("  room qr <idx>                  Print join URI");
      Serial.println("  room read <idx|name> [n]       Show last N messages (def 20)");
      Serial.println("  room clients <idx>             List clients in room");
      Serial.println("  room status <idx>              Show per-client sync status");
      Serial.println("  msgs <idx> [n]                 Show last N posts by room index");
      Serial.println("  nicks <idx>                    Show nicklist for room");
      Serial.println("  say <idx> <text>               Post server-authored message");
      Serial.println("  setperm <hex> <perms>          Set ACL (0=guest 1=ro 2=rw 3=admin)");
      Serial.println();
      Serial.println("  PEERS");
      Serial.println("  -----");
      Serial.println("  peer list                      List known peer nodes");
      Serial.println("  peer add <hex> <name>          Add peer node [serial only]");
      Serial.println("  peer del <idx>                 Remove peer node [serial only]");
      Serial.println("  peer sync                      Trigger manual anti-entropy sync");
      Serial.println();
      Serial.println("  NOTIFY");
      Serial.println("  ------");
      Serial.println("  notify <idx> list              List login-notification targets for room");
      Serial.println("  notify <idx> add <hex64>       Add notification target pubkey");
      Serial.println("  notify <idx> del <hex64>       Remove notification target pubkey");
      Serial.println();
      Serial.println("  WIFI");
      Serial.println("  ----");
      Serial.println("  wifi mode ap|sta               Set WiFi mode");
      Serial.println("  wifi ap ssid <ssid>            Set AP network name");
      Serial.println("  wifi ssid <ssid>               Set STA network SSID");
      Serial.println("  wifi connect                   Connect to STA network");
      Serial.println("  wifi status                    Show WiFi/IP status");
      Serial.println("  mqtt status                    Show MQTT broker status");
      Serial.println();
      Serial.println("  RADIO");
      Serial.println("  -----");
      Serial.println("  stealth on|off|status          Toggle all-room stealth mode");
      Serial.println("  repeater on|off|status         Toggle flood-repeat (independent of stealth)");
      Serial.println("  advert interval [secs]         Get/set advert broadcast interval");
      Serial.println("  set txpower <dBm>              Set TX power (2-22 dBm)");
      Serial.println("  get freq|sf|bw|cr              Get radio parameter");
      Serial.println("  set freq|sf|bw|cr <val>        Set radio parameter");
      Serial.println();
      Serial.println("  SYSTEM");
      Serial.println("  ------");
      Serial.println("  get name|password              Get node name or password");
      Serial.println("  set name <val>                 Set node name");
      Serial.println("  password <val>                 Set admin password");
      Serial.println("  ver                            Show firmware version");
      Serial.println("  board                          Show board info");
      Serial.println("  clock                          Show current time");
      Serial.println("  reboot                         Reboot node");
      Serial.println("  screensaver on|off             Toggle OLED screensaver");
      Serial.println("  menu                           Open interactive settings menu");
      Serial.println();
      Serial.println("  STATS");
      Serial.println("  -----");
      Serial.println("  get stats                      Show node statistics summary");
      Serial.println("  stats                          Brief totals (rooms/posts/contacts/uptime)");
      Serial.println("  stats rooms                    Per-room client counts + post totals");
      Serial.println("  stats users                    Per-user role/hops/RSSI/msgs [all rooms]");
      Serial.println("  stats hist                     24-hour message histogram");
      Serial.println("  stats-core                     Show core stats [serial only]");
      Serial.println("  stats-radio                    Show radio stats [serial only]");
      Serial.println("  neighbors                      List mesh neighbors");
      Serial.println("  name list                      List name<->pubkey entries (*=manual)");
      Serial.println("  name set <8hex> <name>         Pin a name for a 4-byte pubkey prefix");
      Serial.println("  name del <8hex>                Remove a name-table entry");
      Serial.println();
      Serial.println("+---------------------------------------------------------------+");
      Serial.println();
      reply[0] = 0;
    } else {
      strcpy(reply, "rooms|room|msgs|nicks|say|peer|wifi|mqtt|stealth|repeater|get|set|ver|reboot -- type 'help' on serial for full list");
    }
    return;
  }

  // "sync status" — sync diagnostics counters + per-peer timestamps (JES-833)
  if (memcmp(command, "sync status", 11) == 0 && (command[11] == 0 || command[11] == ' ')) {
    if (is_serial) {
      Serial.printf("[SYNC] Globaal: req_sent=%lu dat_recv=%lu posts_recv=%lu posts_sent=%lu\n",
                    (unsigned long)_sync_req_sent, (unsigned long)_sync_dat_recv,
                    (unsigned long)_sync_posts_recv, (unsigned long)_sync_posts_sent);
      for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        Serial.printf("  peer[%d] '%s': req_ts=%lu dat_ts=%lu end_ts=%lu recv=%lu sent=%lu\n",
                      i, peers[i].name,
                      (unsigned long)peers[i].last_syncreq_ts,
                      (unsigned long)peers[i].last_syncdat_ts,
                      (unsigned long)peers[i].last_syncend_ts,
                      (unsigned long)peers[i].sync_posts_recv,
                      (unsigned long)peers[i].sync_posts_sent);
      }
      reply[0] = 0;
    } else {
      snprintf(reply, 160, "sync req=%lu recv=%lu sent=%lu",
               (unsigned long)_sync_req_sent,
               (unsigned long)_sync_posts_recv,
               (unsigned long)_sync_posts_sent);
    }
    return;
  }

  // ---- stats rooms / stats users / stats hist (JES-800) ----
  if (memcmp(command, "stats", 5) == 0 && (command[5] == ' ' || command[5] == 0)) {
    const char* sub = command + 5;
    while (*sub == ' ') sub++;

    if (strcmp(sub, "rooms") == 0) {
      if (is_serial) {
        Serial.printf("Rooms (%d active):\n", _num_active_rooms);
        for (int i = 0; i < MAX_ROOMS; i++) {
          if (!rooms[i].active) continue;
          int nc = rooms[i].acl.getNumClients();
          int admin_c = 0, rw_c = 0, ro_c = 0, guest_c = 0;
          for (int j = 0; j < nc; j++) {
            auto c = rooms[i].acl.getClientByIdx(j);
            switch (c->permissions & PERM_ACL_ROLE_MASK) {
              case PERM_ACL_ADMIN:      admin_c++; break;
              case PERM_ACL_READ_WRITE: rw_c++;    break;
              case PERM_ACL_READ_ONLY:  ro_c++;    break;
              default:                  guest_c++; break;
            }
          }
          Serial.printf("  [%d] '%s'  clients=%d (adm=%d rw=%d ro=%d g=%d)  posts=%d\n",
                        i, rooms[i].name, nc,
                        admin_c, rw_c, ro_c, guest_c,
                        rooms[i].num_posted);
        }
      } else {
        int pos = 0;
        for (int i = 0; i < MAX_ROOMS && pos < 140; i++) {
          if (!rooms[i].active) continue;
          pos += snprintf(reply + pos, 160 - pos, "[%d]%s c=%d p=%d; ",
                          i, rooms[i].name,
                          rooms[i].acl.getNumClients(),
                          rooms[i].num_posted);
        }
        if (pos == 0) strcpy(reply, "no rooms");
      }
      return;
    }

    if (strcmp(sub, "users") == 0) {
      if (is_serial) {
        for (int i = 0; i < MAX_ROOMS; i++) {
          if (!rooms[i].active) continue;
          int nc = rooms[i].acl.getNumClients();
          Serial.printf("Room[%d] '%s':\n", i, rooms[i].name);
          for (int j = 0; j < nc; j++) {
            auto c = rooms[i].acl.getClientByIdx(j);
            char hex[9]; mesh::Utils::toHex(hex, c->id.pub_key, 4); hex[8] = 0;
            const char* nm = resolveName(c->id.pub_key);
            Serial.printf("  <%s> '%s' perm=%d hops=%s rssi=%d snr=%d msgs=%d\n",
                          hex, nm,
                          (c->permissions & PERM_ACL_ROLE_MASK),
                          c->out_path_len == OUT_PATH_UNKNOWN ? "?" : String(c->out_path_len).c_str(),
                          (int)c->last_rssi,
                          (int)c->last_snr,
                          (int)c->msg_count);
          }
        }
      } else {
        // Compact mesh-DM reply; admin-only enforced by caller
        int pos = 0;
        int scope = _active_slot;
        int nc = rooms[scope].acl.getNumClients();
        for (int j = 0; j < nc && pos < 140; j++) {
          auto c = rooms[scope].acl.getClientByIdx(j);
          char hex[9]; mesh::Utils::toHex(hex, c->id.pub_key, 4); hex[8] = 0;
          pos += snprintf(reply + pos, 160 - pos, "<%s> p=%d m=%d; ",
                          hex,
                          (c->permissions & PERM_ACL_ROLE_MASK),
                          (int)c->msg_count);
        }
        if (pos == 0) strcpy(reply, "no users");
      }
      return;
    }

    if (strcmp(sub, "hist") == 0) {
      histAdvance(getRTCClock()->getCurrentTime());
      if (is_serial) {
        Serial.println("Message histogram (last 24h, newest first):");
        for (int b = 0; b < HIST_BUCKETS; b++) {
          uint16_t cnt = getHistBucket(b);
          Serial.printf("  -%2dh: %u\n", b, (unsigned)cnt);
        }
      } else {
        // Compact: last 12 buckets
        int pos = snprintf(reply, 160, "hist:");
        for (int b = 0; b < 12 && pos < 150; b++) {
          pos += snprintf(reply + pos, 160 - pos, "%u,", (unsigned)getHistBucket(b));
        }
        reply[pos > 0 ? pos - 1 : 0] = 0;  // trim trailing comma
      }
      return;
    }

    // "stats" with no sub-command: brief totals
    snprintf(reply, 160, "rooms=%d posts=%lu contacts=%d uptime=%lus",
             _num_active_rooms,
             (unsigned long)getTotalPosts(),
             (int)getTotalContacts(),
             (unsigned long)(uptime_millis / 1000UL));
    return;
  }

  // ---- neighbour discovery (JES-869) ----
  // The 'neighbors' list command is handled by CommonCLI via formatNeighborsReply().
  if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
    return;
  }

  // ---- manual name table (JES-875) ----
  // name list                — show all known name↔pubkey-prefix entries
  // name set <8hex> <name>   — pin a name for a 4-byte pubkey prefix
  // name del <8hex>          — remove a name-table entry
  if (memcmp(command, "name", 4) == 0 && (command[4] == ' ' || command[4] == 0)) {
    const char* sub = command + 4;
    while (*sub == ' ') sub++;
    if (memcmp(sub, "list", 4) == 0) {
      int pos = 0;
      for (int i = 0; i < NAME_TABLE_SIZE && pos < 150; i++) {
        const NameEntry* e = &_names[i];
        if (e->lru_seq == 0) continue;
        char hex[9];
        mesh::Utils::toHex(hex, e->pub_prefix, NAME_KEY_SIZE);
        pos += snprintf(reply + pos, 160 - pos, "%s%s=%s%s",
                        pos ? "\n" : "", hex, e->name, e->manual ? "*" : "");
      }
      if (pos == 0) strcpy(reply, "no names");
      return;
    }
    if (memcmp(sub, "set", 3) == 0 && (sub[3] == ' ' || sub[3] == 0)) {
      const char* a = sub + 3;
      while (*a == ' ') a++;
      // 8 hex chars, a space, then the name
      char hexbuf[9] = {};
      int hn = 0;
      while (hn < 8 && a[hn] &&
             ((a[hn] >= '0' && a[hn] <= '9') ||
              (a[hn] >= 'a' && a[hn] <= 'f') ||
              (a[hn] >= 'A' && a[hn] <= 'F'))) { hexbuf[hn] = a[hn]; hn++; }
      const char* nm = a + hn;
      while (*nm == ' ') nm++;
      uint8_t pfx[NAME_KEY_SIZE];
      if (hn != 8 || !mesh::Utils::fromHex(pfx, NAME_KEY_SIZE, hexbuf)) {
        strcpy(reply, "Err - usage: name set <8hex> <name>");
      } else if (*nm == 0) {
        strcpy(reply, "Err - name required");
      } else if (setNameManual(pfx, nm)) {
        snprintf(reply, 160, "OK - %s = %s", hexbuf, nm);
      } else {
        strcpy(reply, "Err - name table full");
      }
      return;
    }
    if (memcmp(sub, "del", 3) == 0 && (sub[3] == ' ' || sub[3] == 0)) {
      const char* a = sub + 3;
      while (*a == ' ') a++;
      uint8_t pfx[NAME_KEY_SIZE];
      if (strlen(a) < 8 || !mesh::Utils::fromHex(pfx, NAME_KEY_SIZE, a)) {
        strcpy(reply, "Err - usage: name del <8hex>");
      } else if (delName(pfx)) {
        strcpy(reply, "OK - removed");
      } else {
        strcpy(reply, "Err - not found");
      }
      return;
    }
    strcpy(reply, "Err - usage: name list | name set <8hex> <name> | name del <8hex>");
    return;
  }

  // Fall through to shared CommonCLI
  _cli.handleCommand(sender_timestamp, command, reply);
}

/* ------------------------------------------------------------------ */
/*  room * CLI sub-commands                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::handleRoomCommand(char* args, char* reply, bool serial) {
  while (*args == ' ') args++;

  // "room list" — list all rooms
  if (strcmp(args, "list") == 0 || strcmp(args, "ls") == 0) {
    if (serial) {
      Serial.printf("Rooms (%d/%d active):\n", _num_active_rooms, MAX_ROOMS);
      for (int i = 0; i < MAX_ROOMS; i++) {
        Serial.printf("  [%d] %s  name='%s'  stealth=%s  id=", i,
                      rooms[i].active ? "ON " : "OFF", rooms[i].name,
                      rooms[i].stealth ? "on" : "off");
        if (rooms[i].active) {
          mesh::Utils::printHex(Serial, rooms[i].id.pub_key, 4);
          Serial.printf("...  clients=%d  posts=%d",
                        rooms[i].acl.getNumClients(), rooms[i].num_posted);
        }
        Serial.println();
      }
      reply[0] = 0;
    } else {
      // compact mesh-DM reply: "2/16: [0]Room0 c=1 [1]Room1 c=0"
      int pos = sprintf(reply, "%d/%d:", _num_active_rooms, MAX_ROOMS);
      for (int i = 0; i < MAX_ROOMS && pos < 140; i++) {
        if (!rooms[i].active) continue;
        pos += snprintf(reply + pos, 160 - pos, " [%d]%s c=%d",
                        i, rooms[i].name, rooms[i].acl.getNumClients());
      }
    }
    return;
  }

  // "room set <idx> name|pass|guest <value>"
  if (memcmp(args, "set ", 4) == 0) {
    char* p = args + 4;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    // p now points at "name|pass|guest <value>"
    if (memcmp(p, "name ", 5) == 0) {
      StrHelper::strncpy(rooms[idx].name, p + 5, sizeof(rooms[idx].name));
      rooms[idx].config_ts = (uint32_t)getRTCClock()->getCurrentTime();  // JES-860: stamp for LWW
      saveRoomConfig();
      triggerRoomSync(-1);  // JES-856: propagate name change to all peers
      strcpy(reply, "OK");
    } else if (memcmp(p, "pass ", 5) == 0) {
      const char* val = p + 5;
      // Two-step confirmation: require trailing " confirm" (or bare "confirm" for empty password).
      // Serial CLI: run without confirm to see warning, then re-run with confirm.
      // Web path: /api/room/set appends " confirm" after server-side name verification (JES-857).
      size_t vlen = strlen(val);
      bool has_confirm = (vlen == 7 && memcmp(val, "confirm", 7) == 0) ||
                         (vlen > 7 && memcmp(val + vlen - 8, " confirm", 8) == 0);
      if (!has_confirm) {
        if (serial) {
          Serial.printf("\n*** WAARSCHUWING: wachtwoord wijzigen room[%d] '%s' ***\n",
                        idx, rooms[idx].name);
          Serial.println("  Alle companions met het huidige wachtwoord verliezen toegang.");
          Serial.println("  Dit kan niet ongedaan worden gemaakt.");
          Serial.printf("  Voer uit ter bevestiging: room set %d pass %s confirm\n\n",
                        idx, val);
        }
        strcpy(reply, "Aborted - bevestiging vereist (voeg 'confirm' toe aan het einde)");
        return;
      }
      // Strip " confirm" / "confirm" suffix to extract actual password.
      // bare "confirm" (vlen==7) → empty password (clear_pass path).
      // "<val> confirm" (vlen>7) → strip trailing " confirm" (8 chars).
      char pw[sizeof(rooms[idx].password)] = {};
      size_t pw_len = (vlen >= 8) ? vlen - 8 : 0;
      if (pw_len > sizeof(pw) - 1) pw_len = sizeof(pw) - 1;
      memcpy(pw, val, pw_len);
      StrHelper::strncpy(rooms[idx].password, pw, sizeof(rooms[idx].password));
      saveRoomConfig();
      strcpy(reply, "OK");
    } else if (memcmp(p, "guest ", 6) == 0) {
      StrHelper::strncpy(rooms[idx].guest_password, p + 6, sizeof(rooms[idx].guest_password));
      rooms[idx].config_ts = (uint32_t)getRTCClock()->getCurrentTime();  // JES-860: stamp for LWW
      saveRoomConfig();
      triggerRoomSync(-1);  // JES-856: propagate guest_password change to all peers
      strcpy(reply, "OK");
    } else if (memcmp(p, "lat ", 4) == 0) {
      // JES-867: set room advertised latitude (-90..90). 0 = unset/hidden.
      float v = atof(p + 4);
      if (v < -90.0f || v > 90.0f) { strcpy(reply, "Err - lat must be -90..90"); return; }
      rooms[idx].lat = v;
      saveRoomConfig();
      sprintf(reply, "OK - room[%d] lat=%s", idx, StrHelper::ftoa(rooms[idx].lat));
    } else if (memcmp(p, "lon ", 4) == 0) {
      // JES-867: set room advertised longitude (-180..180). 0 = unset/hidden.
      float v = atof(p + 4);
      if (v < -180.0f || v > 180.0f) { strcpy(reply, "Err - lon must be -180..180"); return; }
      rooms[idx].lon = v;
      saveRoomConfig();
      sprintf(reply, "OK - room[%d] lon=%s", idx, StrHelper::ftoa(rooms[idx].lon));
    } else {
      strcpy(reply, "Err - unknown field (use name|pass|guest|lat|lon)");
    }
    return;
  }

  // "room add" — activate next free slot
  if (strcmp(args, "add") == 0) {
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!rooms[i].active) {
        rooms[i].active = true;
        snprintf(rooms[i].name, sizeof(rooms[i].name), "Room%d", i);
        StrHelper::strncpy(rooms[i].password, _prefs.password, sizeof(rooms[i].password));
        rooms[i].guest_password[0] = 0;
        loadOrCreateRoomIdentity(i);
        _num_active_rooms++;
        saveRoomConfig();
        triggerRoomSync(-1);  // JES-856: propagate new room to all peers
        sprintf(reply, "OK - room[%d] added, id=", i);
        // append first 4 hex bytes of pub_key
        char hex[12];
        for (int b = 0; b < 4; b++) {
          snprintf(hex, sizeof(hex), "%02X", rooms[i].id.pub_key[b]);
          strcat(reply, hex);
        }
        return;
      }
    }
    strcpy(reply, "Err - all slots in use");
    return;
  }

  // "room stealth <idx> on|off" — per-room visibility
  if (memcmp(args, "stealth ", 8) == 0) {
    char* p = args + 8;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (strcmp(p, "on") == 0) {
      setRoomStealth(idx, true);
      sprintf(reply, "OK - room[%d] stealth ON", idx);
    } else if (strcmp(p, "off") == 0) {
      setRoomStealth(idx, false);
      sprintf(reply, "OK - room[%d] stealth OFF (visible)", idx);
    } else {
      sprintf(reply, "room[%d] stealth=%s", idx, rooms[idx].stealth ? "on" : "off");
    }
    return;
  }

  // "room del <idx>" — serial-only (destructive; prevents accidental DM deletion)
  if (memcmp(args, "del ", 4) == 0) {
    if (!serial) { strcpy(reply, "Err - room del only allowed via serial CLI"); return; }
    int idx = atoi(args + 4);
    if (idx <= 0 || idx >= MAX_ROOMS) {
      strcpy(reply, "Err - cannot delete room 0 or invalid idx");
      return;
    }
    if (!rooms[idx].active) { strcpy(reply, "Err - room not active"); return; }
    rooms[idx].active = false;
    _num_active_rooms--;
    saveRoomConfig();
    strcpy(reply, "OK");
    return;
  }

  // "room qr <idx>" — print meshcore:// join URI to serial (or put in reply)
  if (memcmp(args, "qr ", 3) == 0 || strcmp(args, "qr") == 0) {
    const char* p = args + 2;
    while (*p == ' ') p++;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - room not active");
      return;
    }
    // Build 64-char hex public key
    char hex64[65] = {};
    for (int b = 0; b < PUB_KEY_SIZE; b++) {
      snprintf(hex64 + b * 2, 3, "%02x", (unsigned int)rooms[idx].id.pub_key[b]);
    }
    // Build URI (room name used raw — ASCII names work fine in serial output)
    char uri[160] = {};
    snprintf(uri, sizeof(uri),
             "meshcore://contact/add?name=%s&public_key=%s&type=3",
             rooms[idx].name, hex64);
    if (serial) {
      Serial.printf("Room[%d] join URI:\n%s\n", idx, uri);
      reply[0] = 0;
    } else {
      strncpy(reply, uri, 159);
      reply[159] = 0;
    }
    return;
  }

  // "room clients <idx>" — list clients in a room with permissions + last seen
  if (memcmp(args, "clients", 7) == 0) {
    const char* p = args + 7;
    while (*p == ' ') p++;
    int idx = (*p >= '0' && *p <= '9') ? atoi(p) : 0;
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - room not active");
      return;
    }
    int n = rooms[idx].acl.getNumClients();
    if (serial) {
      Serial.printf("room[%d] '%s' — %d client(s):\n", idx, rooms[idx].name, n);
      for (int i = 0; i < n; i++) {
        ClientInfo* c = rooms[idx].acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        const char* role = c->isAdmin() ? "admin" :
          ((c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_WRITE ? "rw" :
           (c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_ONLY  ? "ro" : "guest");
        Serial.printf("  [%d] %s  ", i, role);
        mesh::Utils::printHex(Serial, c->id.pub_key, 6);
        Serial.printf("...  last=%lu\n", (unsigned long)c->last_activity);
      }
      reply[0] = 0;
    } else {
      // compact mesh-DM reply: "room[0] 2 clients: [0]admin [1]rw"
      int pos = sprintf(reply, "room[%d] %d clients:", idx, n);
      for (int i = 0; i < n && pos < 140; i++) {
        ClientInfo* c = rooms[idx].acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        const char* role = c->isAdmin() ? "admin" :
          ((c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_WRITE ? "rw" :
           (c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_ONLY  ? "ro" : "guest");
        pos += snprintf(reply + pos, 160 - pos, " [%d]%s", i, role);
      }
    }
    return;
  }

  // "room setperm <room_idx> <hex_pubkey> <perms>" — set ACL permissions in a specific room
  if (memcmp(args, "setperm ", 8) == 0) {
    char* p = args + 8;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - room not active");
      return;
    }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    // p now points to "<hex_pubkey> <perms>"
    char* sp = strchr(p, ' ');
    if (!sp) { strcpy(reply, "Err - usage: room setperm <idx> <hex> <perms>"); return; }
    *sp++ = 0;
    uint8_t pubkey[PUB_KEY_SIZE];
    int hex_len = min((int)strlen(p), PUB_KEY_SIZE * 2);
    if (mesh::Utils::fromHex(pubkey, hex_len / 2, p)) {
      uint8_t perms = atoi(sp);
      if (rooms[idx].acl.applyPermissions(rooms[idx].id, pubkey, hex_len / 2, perms)) {
        rooms[idx].dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
        sprintf(reply, "OK - room[%d] perm set", idx);
      } else {
        strcpy(reply, "Err - pubkey not found or invalid");
      }
    } else {
      strcpy(reply, "Err - bad pubkey hex");
    }
    return;
  }

  // "room status <idx>" — per-client last-contact + unsynced post count
  if (memcmp(args, "status", 6) == 0) {
    const char* p = args + 6;
    while (*p == ' ') p++;
    int idx = (*p >= '0' && *p <= '9') ? atoi(p) : 0;
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - room not active");
      return;
    }
    RoomSlot& slot = rooms[idx];
    int n = slot.acl.getNumClients();
    if (serial) {
      Serial.printf("room[%d] '%s'  posts=%d  clients=%d:\n",
                    idx, slot.name, (int)slot.num_posted, n);
      for (int i = 0; i < n; i++) {
        ClientInfo* c = slot.acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        const char* role = c->isAdmin() ? "admin" :
          ((c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_WRITE ? "rw" :
           (c->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_READ_ONLY  ? "ro" : "guest");
        uint8_t lag = getUnsyncedCount(slot, c);
        Serial.printf("  [%d] %s  ", i, role);
        mesh::Utils::printHex(Serial, c->id.pub_key, 6);
        Serial.printf("...  last_act=%lu  unsynced=%d\n",
                      (unsigned long)c->last_activity, (int)lag);
      }
      reply[0] = 0;
    } else {
      // compact mesh-DM reply with per-client unsynced lag
      int pos = sprintf(reply, "room[%d]'%s' posts=%d clients=%d:",
                        idx, slot.name, (int)slot.num_posted, n);
      for (int i = 0; i < n && pos < 130; i++) {
        ClientInfo* c = slot.acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        uint8_t lag = getUnsyncedCount(slot, c);
        pos += snprintf(reply + pos, 160 - pos, " [%d]lag=%d", i, (int)lag);
      }
    }
    return;
  }

  // "room read <idx|name> [n]" — show last N messages from a room (default 20)
  if (memcmp(args, "read", 4) == 0 && (args[4] == ' ' || args[4] == 0)) {
    const char* rp = args + 4;
    while (*rp == ' ') rp++;

    // parse room identifier (up to next space or end)
    char id_buf[24] = {};
    const char* id_end = rp;
    while (*id_end && *id_end != ' ') id_end++;
    int id_len = (int)(id_end - rp);
    if (id_len > 0 && id_len < (int)sizeof(id_buf)) {
      memcpy(id_buf, rp, id_len);
    }

    // optional count argument
    const char* cnt_p = id_end;
    while (*cnt_p == ' ') cnt_p++;
    int n = (*cnt_p) ? atoi(cnt_p) : 20;
    if (n < 1 || n > 50) n = 20;

    // resolve room: numeric index or name (case-insensitive, prefix ok)
    int ridx = -1;
    bool all_digits = (id_len > 0);
    for (int k = 0; k < id_len; k++) {
      if (id_buf[k] < '0' || id_buf[k] > '9') { all_digits = false; break; }
    }
    if (all_digits && id_len > 0) {
      ridx = atoi(id_buf);
    } else if (id_len > 0) {
      // exact match first
      for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && strcasecmp(rooms[i].name, id_buf) == 0) {
          ridx = i; break;
        }
      }
      // prefix match fallback
      if (ridx < 0) {
        for (int i = 0; i < MAX_ROOMS; i++) {
          if (rooms[i].active &&
              strncasecmp(rooms[i].name, id_buf, id_len) == 0) {
            ridx = i; break;
          }
        }
      }
    }

    if (ridx < 0 || ridx >= MAX_ROOMS || !rooms[ridx].active) {
      strcpy(reply, "Err - room not found"); return;
    }

    // collect and insertion-sort posts for this room by timestamp ascending
    const PostInfo* sorted[MAX_TOTAL_POSTS];
    int cnt = 0;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (_post_pool[i].room_idx == (uint8_t)ridx) sorted[cnt++] = &_post_pool[i];
    }
    for (int i = 1; i < cnt; i++) {
      const PostInfo* key = sorted[i];
      int j = i - 1;
      while (j >= 0 && sorted[j]->post_timestamp > key->post_timestamp) {
        sorted[j+1] = sorted[j]; j--;
      }
      sorted[j+1] = key;
    }

    int start = (cnt > n) ? cnt - n : 0;
    if (serial) {
      Serial.printf("Room [%d] '%s' — showing %d/%d messages:\n",
                    ridx, rooms[ridx].name, cnt - start, cnt);
      for (int i = start; i < cnt; i++) {
        const PostInfo* post = sorted[i];
        uint32_t ts = post->post_timestamp;
        uint32_t hh = (ts % 86400) / 3600;
        uint32_t mm = (ts % 3600) / 60;
        Serial.printf("  [%02u:%02u] %-12s  %s\n",
                      hh, mm, resolveName(post->author.pub_key), post->text);
      }
      reply[0] = 0;
    } else {
      // mesh-DM: compact last-2 summary
      char* p2 = reply;
      char* end = reply + 180;
      p2 += snprintf(p2, end - p2, "r%d %d msgs:", ridx, cnt);
      int show = (cnt > 2) ? 2 : cnt;
      for (int i = cnt - show; i < cnt && p2 < end - 10; i++) {
        p2 += snprintf(p2, end - p2, " <%s>%.28s",
                       resolveName(sorted[i]->author.pub_key), sorted[i]->text);
      }
    }
    return;
  }

  // "room export <idx>" — serial-only: print 64-byte (128 hex chars) private key to serial.
  // SECURITY: key is NEVER written to reply, web-UI, or any log; stack copy is scrubbed.
  if (memcmp(args, "export", 6) == 0 && (args[6] == ' ' || args[6] == 0)) {
    if (!serial) { strcpy(reply, "Err - room export only allowed via serial CLI"); return; }
    const char* p = args + 6;
    while (*p == ' ') p++;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    if (!rooms[idx].active) { strcpy(reply, "Err - room not active"); return; }

    uint8_t prv[PRV_KEY_SIZE];
    rooms[idx].id.writeTo(prv, PRV_KEY_SIZE);

    Serial.printf("\n=== PRIVATE KEY: room[%d] '%s' ===\n", idx, rooms[idx].name);
    Serial.println("WARNING: Sharing this key grants full room access. Use only in a");
    Serial.println("         controlled, trusted environment. Never paste in logs or issues.");
    Serial.print("PRIVKEY: ");
    mesh::Utils::printHex(Serial, prv, PRV_KEY_SIZE);
    Serial.println();
    Serial.println("=== END PRIVATE KEY ===\n");

    memset(prv, 0, PRV_KEY_SIZE);  // scrub private key from stack
    reply[0] = 0;
    return;
  }

  // "room import <idx> <128hex>" — serial-only: import 64-byte private key into room slot.
  // Accepts exactly PRV_KEY_SIZE*2 (128) hex chars. Derives pubkey via ed25519_derive_pub.
  // If the slot is inactive it is activated (with a default name). If active, the key is
  // replaced in-place. Peer ECDH secrets are invalidated and VV is reset.
  // SECURITY: key material is scrubbed from the stack after use; never echoed in reply.
  if (memcmp(args, "import", 6) == 0 && (args[6] == ' ' || args[6] == 0)) {
    if (!serial) { strcpy(reply, "Err - room import only allowed via serial CLI"); return; }
    const char* p = args + 6;
    while (*p == ' ') p++;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    // advance past idx digits
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    // p now points at hex string — must be exactly PRV_KEY_SIZE*2 chars (128)
    size_t hexlen = strlen(p);
    if (hexlen != (size_t)(PRV_KEY_SIZE * 2)) {
      snprintf(reply, 160, "Err - key must be exactly %d hex chars (%d bytes), got %d",
               PRV_KEY_SIZE * 2, PRV_KEY_SIZE, (int)hexlen);
      return;
    }
    uint8_t prv[PRV_KEY_SIZE];
    if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, p)) {
      strcpy(reply, "Err - invalid hex chars in key");
      memset(prv, 0, PRV_KEY_SIZE);
      return;
    }
    if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
      strcpy(reply, "Err - key validation failed (pub prefix 00/FF, or ECDH mismatch)");
      memset(prv, 0, PRV_KEY_SIZE);
      return;
    }
    // Load: readFrom(src, PRV_KEY_SIZE) stores prv_key and derives pub_key via ed25519_derive_pub
    rooms[idx].id.readFrom(prv, PRV_KEY_SIZE);
    memset(prv, 0, PRV_KEY_SIZE);  // scrub key from stack

    // Activate slot if not already active
    if (!rooms[idx].active) {
      rooms[idx].active = true;
      snprintf(rooms[idx].name, sizeof(rooms[idx].name), "Room%d", idx);
      StrHelper::strncpy(rooms[idx].password, _prefs.password, sizeof(rooms[idx].password));
      rooms[idx].guest_password[0] = 0;
      _num_active_rooms++;
    }

    saveRoomIdentity(idx);
    saveRoomConfig();

    // Invalidate all peer ECDH shared secrets (room key changed — force ECDH recalc)
    for (int pi = 0; pi < MAX_PEERS; pi++) {
      peers[pi].secret_valid = false;
    }

    // Reset VV — this is now a different origin; old VV entries are stale
    memset(rooms[idx].vv, 0, sizeof(rooms[idx].vv));

    // Reply shows pub prefix only — private key is NEVER echoed
    int pos = snprintf(reply, 160, "OK - room[%d] key imported. pub prefix=", idx);
    for (int b = 0; b < 4 && pos < 156; b++) {
      pos += snprintf(reply + pos, 160 - pos, "%02X", rooms[idx].id.pub_key[b]);
    }
    return;
  }

  // "room rekey <idx>" / "room rekey <idx> confirm" — serial-only, two-step key rotation
  if (memcmp(args, "rekey", 5) == 0 && (args[5] == ' ' || args[5] == 0)) {
    if (!serial) { strcpy(reply, "Err - room rekey only allowed via serial CLI"); return; }
    const char* p = args + 5;
    while (*p == ' ') p++;
    int idx = atoi(p);
    // advance past idx digits
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    bool confirmed = (strcmp(p, "confirm") == 0);

    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    if (!rooms[idx].active) { strcpy(reply, "Err - room not active"); return; }

    if (!confirmed) {
      // Step 1: print warning and instruct operator to re-run with 'confirm'
      Serial.printf("\n*** WARNING: room rekey %d ***\n", idx);
      if (idx == 0) {
        Serial.println("  Room 0 = node identity. Rekeying BREAKS all peer links.");
        Serial.println("  All companion nodes must re-add this room-server.");
      } else {
        Serial.printf("  Room[%d] '%s': existing join URIs/QR codes become invalid.\n",
                      idx, rooms[idx].name);
        Serial.println("  Companions must re-import the room after rekeying.");
      }
      Serial.println("  Make a new backup afterwards.");
      Serial.printf("  To proceed, run: room rekey %d confirm\n\n", idx);
      reply[0] = 0;
      return;
    }

    // Step 2: confirmed — perform key rotation
    rekeyRoom(idx);
    int pos = snprintf(reply, 160, "OK - room[%d] rekeyed. New pub prefix=", idx);
    for (int b = 0; b < 4 && pos < 156; b++) {
      pos += snprintf(reply + pos, 160 - pos, "%02X", rooms[idx].id.pub_key[b]);
    }
    return;
  }

  // "room delpost <idx> <origin_id_hex8> <post_ts>" — serial-only, with y/N confirmation
  if (memcmp(args, "delpost", 7) == 0 && (args[7] == ' ' || args[7] == 0)) {
    if (!serial) { strcpy(reply, "Err - room delpost only allowed via serial CLI"); return; }
    const char* p = args + 7;
    while (*p == ' ') p++;
    int idx = atoi(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    // origin_id: must be exactly 8 hex chars
    if (strlen(p) < 8 || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) ||
        !isxdigit((unsigned char)p[2]) || !isxdigit((unsigned char)p[3]) ||
        !isxdigit((unsigned char)p[4]) || !isxdigit((unsigned char)p[5]) ||
        !isxdigit((unsigned char)p[6]) || !isxdigit((unsigned char)p[7]) ||
        (p[8] != ' ' && p[8] != 0)) {
      strcpy(reply, "Err - origin_id must be exactly 8 hex chars");
      return;
    }
    uint8_t oid[4];
    for (int b = 0; b < 4; b++) {
      char hb[3] = { p[b * 2], p[b * 2 + 1], 0 };
      oid[b] = (uint8_t)strtoul(hb, nullptr, 16);
    }
    p += 8;
    while (*p == ' ') p++;
    if (*p == 0) { strcpy(reply, "Err - missing post_ts"); return; }
    uint32_t post_ts = (uint32_t)strtoul(p, nullptr, 10);
    if (post_ts == 0) { strcpy(reply, "Err - invalid post_ts"); return; }
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - invalid or inactive room idx"); return;
    }
    // Two-step confirmation
    Serial.printf("Delete post ts=%lu from room[%d]? [y/N]: ", (unsigned long)post_ts, idx);
    // Read confirmation from serial (blocking short wait — serial CLI is synchronous)
    unsigned long deadline = millis() + 10000;
    char conf = 'N';
    while (millis() < deadline) {
      if (Serial.available()) { conf = (char)Serial.read(); break; }
      delay(10);
    }
    Serial.println(conf);
    if (conf != 'y' && conf != 'Y') { strcpy(reply, "Aborted"); return; }
    bool found = handleDeletePost((uint8_t)idx, oid, post_ts);
    strcpy(reply, found ? "OK: post deleted" : "Error: post not found");
    return;
  }

  // "room export <idx>" — serial-only; prints 64-byte private key as 128-char hex
  // Used to share a room identity to another node so both host the same room (JES-832).
  // SECURITY: private key is printed in clear; use only on trusted console.
  if (memcmp(args, "export ", 7) == 0) {
    if (!serial) { strcpy(reply, "Err - room export only allowed via serial CLI"); return; }
    int idx = atoi(args + 7);
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) {
      strcpy(reply, "Err - room not active");
      return;
    }
    uint8_t prv_buf[PRV_KEY_SIZE];
    size_t prv_len = rooms[idx].id.writeTo(prv_buf, sizeof(prv_buf));
    if (prv_len < PRV_KEY_SIZE) { strcpy(reply, "Err - writeTo failed"); return; }
    char hex[PRV_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, prv_buf, PRV_KEY_SIZE);
    hex[PRV_KEY_SIZE * 2] = 0;
    Serial.printf("room[%d] '%s' private key (128 hex chars) — handle with care:\n%s\n",
                  idx, rooms[idx].name, hex);
    reply[0] = 0;
    return;
  }

  // "room import <idx> <hex128>" — serial-only; import a private key from another node.
  // After import both nodes share the same room identity → sync works via room_hash match.
  // SECURITY: serial-only; resets VV; invalidates peer ECDH secrets for room 0.
  if (memcmp(args, "import ", 7) == 0) {
    if (!serial) { strcpy(reply, "Err - room import only allowed via serial CLI"); return; }
    char* p = args + 7;
    int idx = atoi(p);
    if (idx < 0 || idx >= MAX_ROOMS) { strcpy(reply, "Err - invalid room idx"); return; }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if ((int)strlen(p) < PRV_KEY_SIZE * 2) {
      strcpy(reply, "Err - need 128-char hex private key (from: room export <idx>)");
      return;
    }
    uint8_t prv[PRV_KEY_SIZE];
    if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, p)) {
      strcpy(reply, "Err - invalid hex");
      return;
    }
    if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
      strcpy(reply, "Err - invalid private key");
      return;
    }
    // Activate slot if importing into an empty slot
    if (!rooms[idx].active) {
      rooms[idx].active = true;
      _num_active_rooms++;
      snprintf(rooms[idx].name, sizeof(rooms[idx].name), "Room%d", idx);
      StrHelper::strncpy(rooms[idx].password, _prefs.password, sizeof(rooms[idx].password));
      rooms[idx].guest_password[0] = 0;
      saveRoomConfig();
    }
    rooms[idx].id.readFrom(prv, PRV_KEY_SIZE);
    saveRoomIdentity(idx);
    // Reset VV — fresh start with new/shared identity
    memset(rooms[idx].vv, 0, sizeof(rooms[idx].vv));
    if (idx == 0) {
      self_id = rooms[0].id;
      for (int pi = 0; pi < MAX_PEERS; pi++) peers[pi].secret_valid = false;
    }
    Serial.printf("room[%d] identity imported. New pub prefix=", idx);
    for (int b = 0; b < 4; b++) Serial.printf("%02X", rooms[idx].id.pub_key[b]);
    Serial.println();
    strcpy(reply, "OK");
    return;
  }

  if (serial) {
    Serial.println("Err - unknown room sub-command. Valid: list|add|del|set|stealth|qr|clients|setperm|status|read|export|import|rekey|delpost");
    reply[0] = 0;
  } else {
    strcpy(reply, "Err - bad room command");
  }
}

/* ------------------------------------------------------------------ */
/*  Peer config persistence                                             */
/* ------------------------------------------------------------------ */
#define PEER_CFG_PATH "/peer_cfg"

void MultiRoomMesh::savePeerConfig() {
  if (!_fs) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PEER_CFG_PATH, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(PEER_CFG_PATH);
  File f = _fs->open(PEER_CFG_PATH, FILE_O_WRITE);
#else
  File f = _fs->open(PEER_CFG_PATH, "w", true);
#endif
  if (!f) return;
  uint8_t n = MAX_PEERS;
  f.write(&n, 1);
  for (int i = 0; i < MAX_PEERS; i++) {
    uint8_t active = peers[i].active ? 1 : 0;
    f.write(&active, 1);
    f.write((uint8_t*)peers[i].name, sizeof(peers[i].name));
    f.write(peers[i].pub_key, PUB_KEY_SIZE);
    f.write((uint8_t*)&peers[i].last_contact, 4);
  }
  f.close();
}

void MultiRoomMesh::loadPeerConfig() {
  if (!_fs || !_fs->exists(PEER_CFG_PATH)) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PEER_CFG_PATH, "r");
#else
  File f = _fs->open(PEER_CFG_PATH);
#endif
  if (!f) return;
  uint8_t n = 0;
  if (f.read(&n, 1) != 1) { f.close(); return; }
  if (n > MAX_PEERS) n = MAX_PEERS;
  _num_peers = 0;
  for (int i = 0; i < n; i++) {
    uint8_t active = 0;
    if (f.read(&active, 1) != 1) break;
    if (f.read((uint8_t*)peers[i].name, sizeof(peers[i].name)) != (int)sizeof(peers[i].name)) break;
    if (f.read(peers[i].pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE) break;
    if (f.read((uint8_t*)&peers[i].last_contact, 4) != 4) break;
    peers[i].active = (active != 0);
    if (peers[i].active) _num_peers++;
  }
  f.close();
}

/* ------------------------------------------------------------------ */
/*  Post pool persistence (JES-787, updated Phase 5)                    */
/*  Binary layout per slot (193 bytes):                                 */
/*    [32: author pub_key][4: post_timestamp][152: text]                */
/*    [1: room_idx][4: origin_id]                                       */
/*  File header (6 bytes):                                              */
/*    [4: magic 'POST'][1: version=2][1: num_slots=MAX_TOTAL_POSTS]    */
/* ------------------------------------------------------------------ */
#define POST_LOG_PATH    "/post_log"
#define POST_LOG_TMP     "/post_log.tmp"
#define POST_LOG_MAGIC_0 0x50   // 'P'
#define POST_LOG_MAGIC_1 0x4F   // 'O'
#define POST_LOG_MAGIC_2 0x53   // 'S'
#define POST_LOG_MAGIC_3 0x54   // 'T'
#define POST_LOG_VERSION 2      // bumped: added origin_id[4] per slot
// v1 slot = 32+4+152+1 = 189 bytes (no origin_id); v2 slot = 193 bytes
#define POST_LOG_V1_SLOT_SIZE  (PUB_KEY_SIZE + 4 + (MAX_POST_TEXT_LEN + 1) + 1)

void MultiRoomMesh::savePostPool() {
  if (!_fs) return;
  // Write to .tmp first; rename atomically so a power-cut never corrupts the live file.
#if defined(RP2040_PLATFORM)
  File f = _fs->open(POST_LOG_TMP, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(POST_LOG_TMP);
  File f = _fs->open(POST_LOG_TMP, FILE_O_WRITE);
#else
  File f = _fs->open(POST_LOG_TMP, "w", true);
#endif
  if (!f) return;

  // Header
  uint8_t hdr[6] = {POST_LOG_MAGIC_0, POST_LOG_MAGIC_1,
                    POST_LOG_MAGIC_2, POST_LOG_MAGIC_3,
                    POST_LOG_VERSION, (uint8_t)MAX_TOTAL_POSTS};
  f.write(hdr, 6);

  // All pool slots (field-by-field to avoid struct padding issues)
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    const PostInfo& p = _post_pool[i];
    f.write(p.author.pub_key, PUB_KEY_SIZE);
    f.write((const uint8_t*)&p.post_timestamp, 4);
    f.write((const uint8_t*)p.text, MAX_POST_TEXT_LEN + 1);
    f.write(&p.room_idx, 1);
    f.write(p.origin_id, 4);   // Phase 5
  }
  f.close();

  // Atomic commit: remove old file then rename tmp into place.
  // SPIFFS has no true rename; remove+rename is the closest atomic-ish operation.
  _fs->remove(POST_LOG_PATH);
  _fs->rename(POST_LOG_TMP, POST_LOG_PATH);
}

void MultiRoomMesh::loadPostPool() {
  if (!_fs) return;

  // Remove any leftover .tmp from a prior interrupted write (power-cut recovery).
  if (_fs->exists(POST_LOG_TMP)) _fs->remove(POST_LOG_TMP);

#if defined(RP2040_PLATFORM)
  if (!_fs->exists(POST_LOG_PATH)) return;
  File f = _fs->open(POST_LOG_PATH, "r");
#else
  if (!_fs->exists(POST_LOG_PATH)) return;
  File f = _fs->open(POST_LOG_PATH);
#endif
  if (!f) return;

  // Validate magic
  uint8_t hdr[6];
  if (f.read(hdr, 6) != 6) { f.close(); return; }
  if (hdr[0] != POST_LOG_MAGIC_0 || hdr[1] != POST_LOG_MAGIC_1 ||
      hdr[2] != POST_LOG_MAGIC_2 || hdr[3] != POST_LOG_MAGIC_3) {
    f.close();
    return;   // unrecognised file — start empty (fail-safe, no crash)
  }

  uint8_t file_ver   = hdr[4];
  int     stored_cnt = (int)(uint8_t)hdr[5];  // slots written to disk
  bool    need_resave = false;

  if (file_ver == 1) {
    // v1 layout: no origin_id field — 189 bytes per slot instead of 193.
    // Migrate: read v1 slots, zero-fill origin_id, resave as v2 afterwards.
    int load_cnt = (stored_cnt < MAX_TOTAL_POSTS) ? stored_cnt : MAX_TOTAL_POSTS;
    for (int i = 0; i < load_cnt; i++) {
      PostInfo& p = _post_pool[i];
      if (f.read(p.author.pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE) break;
      if (f.read((uint8_t*)&p.post_timestamp, 4) != 4) break;
      if (f.read((uint8_t*)p.text, MAX_POST_TEXT_LEN + 1) != (int)(MAX_POST_TEXT_LEN + 1)) break;
      if (f.read(&p.room_idx, 1) != 1) break;
      memset(p.origin_id, 0, 4);  // unknown origin — zero is safe default
      p.msg_id = 0;               // JES-861: not persisted — unknown for old posts
      // Prune posts for rooms that are no longer active
      if (p.room_idx != 0xFF &&
          (p.room_idx >= MAX_ROOMS || !rooms[p.room_idx].active)) {
        memset(&p, 0, sizeof(PostInfo));
        p.room_idx = 0xFF;
      }
    }
    need_resave = true;  // write back as v2 so next boot skips migration
  } else if (file_ver == POST_LOG_VERSION) {
    // Current v2 layout — load min(stored, MAX_TOTAL_POSTS) so a smaller
    // MAX_TOTAL_POSTS build doesn't crash; excess slots are silently dropped.
    int load_cnt = (stored_cnt < MAX_TOTAL_POSTS) ? stored_cnt : MAX_TOTAL_POSTS;
    for (int i = 0; i < load_cnt; i++) {
      PostInfo& p = _post_pool[i];
      if (f.read(p.author.pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE) break;
      if (f.read((uint8_t*)&p.post_timestamp, 4) != 4) break;
      if (f.read((uint8_t*)p.text, MAX_POST_TEXT_LEN + 1) != (int)(MAX_POST_TEXT_LEN + 1)) break;
      if (f.read(&p.room_idx, 1) != 1) break;
      if (f.read(p.origin_id, 4) != 4) break;  // Phase 5
      p.msg_id = 0;                            // JES-861: not persisted — unknown for old posts
      // Prune posts for rooms that are no longer active
      if (p.room_idx != 0xFF &&
          (p.room_idx >= MAX_ROOMS || !rooms[p.room_idx].active)) {
        memset(&p, 0, sizeof(PostInfo));
        p.room_idx = 0xFF;
      }
    }
    if (stored_cnt != MAX_TOTAL_POSTS) need_resave = true;  // normalise slot count
  } else {
    // Unknown/future version — fail-safe: start empty, no crash.
    f.close();
    return;
  }
  f.close();

  // Recount num_posted and rebuild per-room VV from restored pool
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    uint16_t cnt = 0;
    for (int k = 0; k < MAX_TOTAL_POSTS; k++) {
      if (_post_pool[k].room_idx == (uint8_t)i) {
        cnt++;
        // Phase 5: rebuild VV from persisted posts
        vvUpdate(rooms[i], _post_pool[k].origin_id, _post_pool[k].post_timestamp);
      }
    }
    rooms[i].num_posted = cnt;
  }

  // Write migrated/normalised data back immediately so next boot needs no migration.
  if (need_resave) savePostPool();
}

/* ------------------------------------------------------------------ */
/*  Name resolution table (JES-798)                                     */
/* ------------------------------------------------------------------ */
#define NAMES_PATH "/names"
#define NAMES_MAGIC_0 0x4E   // 'N'
#define NAMES_MAGIC_1 0x4D   // 'M'
#define NAMES_VERSION    3   // v3: added manual flag (JES-875); v2 = 4-byte prefix, no flag

void MultiRoomMesh::saveNameTable() {
  if (!_fs) return;
  File f = _fs->open(NAMES_PATH, "w");
  if (!f) return;
  uint8_t hdr[3] = { NAMES_MAGIC_0, NAMES_MAGIC_1, NAMES_VERSION };
  f.write(hdr, 3);
  f.write((uint8_t)NAME_TABLE_SIZE);
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    f.write(_names[i].pub_prefix, NAME_KEY_SIZE);
    f.write((const uint8_t*)_names[i].name, sizeof(_names[i].name));
    f.write((const uint8_t*)&_names[i].lru_seq, 4);
    f.write((uint8_t)(_names[i].manual ? 1 : 0));   // JES-875
  }
  f.close();
}

void MultiRoomMesh::loadNameTable() {
  if (!_fs) return;
  if (!_fs->exists(NAMES_PATH)) return;
  File f = _fs->open(NAMES_PATH);
  if (!f) return;
  uint8_t hdr[4];
  if (f.read(hdr, 4) != 4 ||
      hdr[0] != NAMES_MAGIC_0 || hdr[1] != NAMES_MAGIC_1 ||
      hdr[3] != (uint8_t)NAME_TABLE_SIZE) {
    f.close(); return;
  }
  // Accept current (v3) and prior (v2) layouts; v2 lacks the per-entry manual
  // flag (JES-875) — migrate in place by defaulting manual=false, so
  // advert-learned names survive a firmware upgrade instead of being wiped.
  uint8_t ver = hdr[2];
  if (ver != NAMES_VERSION && ver != 2) { f.close(); return; }
  uint32_t max_seq = 0;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (f.read(_names[i].pub_prefix, NAME_KEY_SIZE) != NAME_KEY_SIZE) break;
    if (f.read((uint8_t*)_names[i].name, sizeof(_names[i].name)) != sizeof(_names[i].name)) break;
    if (f.read((uint8_t*)&_names[i].lru_seq, 4) != 4) break;
    _names[i].manual = false;
    if (ver >= 3) {
      uint8_t m = 0;
      if (f.read(&m, 1) != 1) break;
      _names[i].manual = (m != 0);
    }
    _names[i].name[sizeof(_names[i].name) - 1] = 0;  // ensure NUL
    if (_names[i].lru_seq > max_seq) max_seq = _names[i].lru_seq;
  }
  f.close();
  _name_lru_ctr = max_seq;
}

/* ------------------------------------------------------------------ */
/*  Notify target persistence + management (JES-834)                   */
/* ------------------------------------------------------------------ */
#define NOTIFY_CFG_PATH   "/notify_cfg"
#define NOTIFY_MAGIC_0    0x4E  // 'N'
#define NOTIFY_MAGIC_1    0x54  // 'T'
#define NOTIFY_VERSION    0x01

void MultiRoomMesh::saveNotifyTargets() {
  if (!_fs) return;
  File f = _fs->open(NOTIFY_CFG_PATH, "w");
  if (!f) return;
  uint8_t hdr[4] = { NOTIFY_MAGIC_0, NOTIFY_MAGIC_1, NOTIFY_VERSION, (uint8_t)MAX_ROOMS };
  f.write(hdr, 4);
  for (int r = 0; r < MAX_ROOMS; r++) {
    f.write(&_notify_target_count[r], 1);
    for (int i = 0; i < _notify_target_count[r]; i++) {
      f.write(_notify_targets[r][i], PUB_KEY_SIZE);
    }
  }
  f.close();
}

void MultiRoomMesh::loadNotifyTargets() {
  if (!_fs || !_fs->exists(NOTIFY_CFG_PATH)) return;
  File f = _fs->open(NOTIFY_CFG_PATH);
  if (!f) return;
  uint8_t hdr[4];
  if (f.read(hdr, 4) != 4 ||
      hdr[0] != NOTIFY_MAGIC_0 || hdr[1] != NOTIFY_MAGIC_1 ||
      hdr[2] != NOTIFY_VERSION) {
    f.close(); return;  // corrupt or version mismatch — start empty
  }
  uint8_t rooms_saved = hdr[3];
  if (rooms_saved > MAX_ROOMS) rooms_saved = MAX_ROOMS;
  for (int r = 0; r < rooms_saved; r++) {
    uint8_t cnt = 0;
    if (f.read(&cnt, 1) != 1) break;
    if (cnt > MAX_NOTIFY_TARGETS) cnt = MAX_NOTIFY_TARGETS;
    _notify_target_count[r] = 0;
    for (int i = 0; i < cnt; i++) {
      if (f.read(_notify_targets[r][i], PUB_KEY_SIZE) != PUB_KEY_SIZE) {
        f.close(); return;
      }
      _notify_target_count[r]++;
    }
  }
  f.close();
}

bool MultiRoomMesh::addNotifyTarget(int room_idx, const uint8_t* pub_key) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS) return false;
  // Duplicate check
  for (int i = 0; i < _notify_target_count[room_idx]; i++) {
    if (memcmp(_notify_targets[room_idx][i], pub_key, PUB_KEY_SIZE) == 0) return true;
  }
  if (_notify_target_count[room_idx] >= MAX_NOTIFY_TARGETS) return false;
  memcpy(_notify_targets[room_idx][_notify_target_count[room_idx]], pub_key, PUB_KEY_SIZE);
  _notify_target_count[room_idx]++;
  saveNotifyTargets();
  return true;
}

bool MultiRoomMesh::delNotifyTarget(int room_idx, const uint8_t* pub_key) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS) return false;
  for (int i = 0; i < _notify_target_count[room_idx]; i++) {
    if (memcmp(_notify_targets[room_idx][i], pub_key, PUB_KEY_SIZE) == 0) {
      // Shift remaining entries down
      for (int j = i + 1; j < _notify_target_count[room_idx]; j++) {
        memcpy(_notify_targets[room_idx][j - 1], _notify_targets[room_idx][j], PUB_KEY_SIZE);
      }
      _notify_target_count[room_idx]--;
      saveNotifyTargets();
      return true;
    }
  }
  return false;
}

int MultiRoomMesh::getNotifyTargetCount(int room_idx) const {
  if (room_idx < 0 || room_idx >= MAX_ROOMS) return 0;
  return _notify_target_count[room_idx];
}

const uint8_t* MultiRoomMesh::getNotifyTarget(int room_idx, int i) const {
  if (room_idx < 0 || room_idx >= MAX_ROOMS) return nullptr;
  if (i < 0 || i >= _notify_target_count[room_idx]) return nullptr;
  return _notify_targets[room_idx][i];
}

void MultiRoomMesh::onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id,
                                  uint32_t ts,
                                  const uint8_t* app_data, size_t app_data_len) {
  AdvertDataParser parser(app_data, (uint8_t)app_data_len);

  // Track direct-hop nodes (zero path hops) as neighbours (JES-869), recording
  // the advertised location if present (JES-868) so the web neighbour view can
  // show where each node is.
  if (pkt && pkt->getPathHashCount() == 0 && parser.isValid()) {
    if (parser.hasLatLon()) {
      putNeighbour(id, ts, pkt->getSNR(), 0, true,
                   parser.getIntLat(), parser.getIntLon());
    } else {
      putNeighbour(id, ts, pkt->getSNR());
    }
  }

  if (!parser.isValid() || !parser.hasName()) return;
  const char* adv_name = parser.getName();
  if (!adv_name || adv_name[0] == 0) return;
  DLOG((uint32_t)(millis()/1000),
       "RX advert id=%02x%02x%02x%02x name=%.30s",
       (unsigned)id.pub_key[0], (unsigned)id.pub_key[1],
       (unsigned)id.pub_key[2], (unsigned)id.pub_key[3],
       adv_name);

  // Find existing entry or lowest-seq victim. Manual entries (JES-875) are
  // pinned: never overwritten by an advert, never chosen as eviction victim.
  int victim = -1;
  uint32_t min_seq = UINT32_MAX;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq == 0) {
      // Empty slot — use immediately
      victim = i;
      break;
    }
    if (memcmp(_names[i].pub_prefix, id.pub_key, NAME_KEY_SIZE) == 0) {
      if (_names[i].manual) return;   // operator-set name wins; leave it alone
      // Update existing entry
      StrHelper::strncpy(_names[i].name, adv_name, sizeof(_names[i].name));
      _names[i].lru_seq = ++_name_lru_ctr;
      saveNameTable();
      return;
    }
    if (!_names[i].manual && _names[i].lru_seq < min_seq) {
      min_seq = _names[i].lru_seq; victim = i;
    }
  }

  if (victim < 0) return;  // table full of manual entries — nothing to evict
  // Fill victim slot
  memcpy(_names[victim].pub_prefix, id.pub_key, NAME_KEY_SIZE);
  StrHelper::strncpy(_names[victim].name, adv_name, sizeof(_names[victim].name));
  _names[victim].lru_seq = ++_name_lru_ctr;
  _names[victim].manual = false;
  saveNameTable();
}

const char* MultiRoomMesh::resolveName(const uint8_t* pubkey) {
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq == 0) continue;
    if (memcmp(_names[i].pub_prefix, pubkey, NAME_KEY_SIZE) == 0) {
      return _names[i].name;
    }
  }
  // Fallback: 8-char hex prefix (uses static buffer — single-threaded ESP32 OK)
  static char hex_buf[9];
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    hex_buf[i * 2]     = hx[pubkey[i] >> 4];
    hex_buf[i * 2 + 1] = hx[pubkey[i] & 0x0f];
  }
  hex_buf[8] = 0;
  return hex_buf;
}

/* ------------------------------------------------------------------ */
/*  Neighbour discovery (JES-869)                                       */
/* ------------------------------------------------------------------ */

#define CTL_TYPE_NODE_DISCOVER_REQ  0x80
#define CTL_TYPE_NODE_DISCOVER_RESP 0x90

void MultiRoomMesh::putNeighbour(const mesh::Identity& id, uint32_t timestamp,
                                 float snr, uint8_t node_type,
                                 bool has_loc, int32_t lat, int32_t lon) {
#if MAX_NEIGHBOURS
  // Find existing neighbour, else evict the least-recently-heard slot.
  uint32_t oldest_ts = 0xFFFFFFFF;
  NeighbourInfo* slot = &_neighbours[0];
  bool found = false;
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (id.matches(_neighbours[i].id)) { slot = &_neighbours[i]; found = true; break; }
    if (_neighbours[i].heard_timestamp < oldest_ts) {
      oldest_ts = _neighbours[i].heard_timestamp;
      slot = &_neighbours[i];
    }
  }
  if (!found) { slot->has_loc = false; slot->lat = 0; slot->lon = 0; }
  slot->id = id;
  slot->advert_timestamp = timestamp;
  slot->heard_timestamp = getRTCClock()->getCurrentTime();
  slot->snr = (int8_t)(snr * 4);
  if (node_type) slot->node_type = node_type;  // keep prior type if unknown (0)
  if (has_loc) {  // JES-868: keep last known location if this advert carried none
    slot->has_loc = true;
    slot->lat = lat;
    slot->lon = lon;
  }
#else
  (void)id; (void)timestamp; (void)snr; (void)node_type;
  (void)has_loc; (void)lat; (void)lon;
#endif
}

void MultiRoomMesh::formatNeighborsReply(char* reply) {
  char* dp = reply;
#if MAX_NEIGHBOURS
  // Collect non-empty entries, sort newest-heard first.
  int cnt = 0;
  NeighbourInfo* sorted[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (_neighbours[i].heard_timestamp > 0)
      sorted[cnt++] = &_neighbours[i];
  }
  std::sort(sorted, sorted + cnt, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp;
  });
  uint32_t now = getRTCClock()->getCurrentTime();
  for (int i = 0; i < cnt && dp - reply < (int)(MAX_PACKET_PAYLOAD - 40); i++) {
    if (i > 0) *dp++ = '\n';
    char hex[10];
    mesh::Utils::toHex(hex, sorted[i]->id.pub_key, 4);
    uint32_t ago = (now >= sorted[i]->heard_timestamp)
                   ? now - sorted[i]->heard_timestamp : 0;
    const char* nm = resolveName(sorted[i]->id.pub_key);
    sprintf(dp, "%s(%s):%us:snr%.1f",
            hex, nm, (unsigned)ago, sorted[i]->snr / 4.0f);
    while (*dp) dp++;
  }
#endif
  if (dp == reply) { strcpy(dp, "-none-"); dp += 6; }
  *dp = 0;
}

void MultiRoomMesh::removeNeighbor(const uint8_t* pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (_neighbours[i].heard_timestamp > 0 &&
        memcmp(_neighbours[i].id.pub_key, pubkey, key_len) == 0) {
      memset(&_neighbours[i], 0, sizeof(NeighbourInfo));
    }
  }
#else
  (void)pubkey; (void)key_len;
#endif
}

void MultiRoomMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ;      // prefix_only = 0
  data[1] = (1 << 2) | (1 << 3);             // ADV_TYPE_REPEATER(2) | ADV_TYPE_ROOM(3)
  getRNG()->random(&data[2], 4);             // random match tag
  memcpy(&_pending_discover_tag, &data[2], 4);
  _pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);
  auto pkt = createControlData(data, sizeof(data));
  if (pkt) sendZeroHop(pkt);
}

void MultiRoomMesh::onControlDataRecv(mesh::Packet* packet) {
  if (packet->payload_len < 1) return;
  uint8_t type = packet->payload[0] & 0xF0;

  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6) {
    // Rate-limit our responses to protect shared airtime (JES-869).
    if (!_discover_limiter.allow(getRTCClock()->getCurrentTime())) return;
    uint8_t filter = packet->payload[1];
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    // Respond only if the requester is looking for room servers (ADV_TYPE_ROOM = 3).
    if (filter & (1 << 3)) {
      uint8_t resp[6 + PUB_KEY_SIZE];
      resp[0] = CTL_TYPE_NODE_DISCOVER_RESP | 3;  // low 4 bits = ADV_TYPE_ROOM
      resp[1] = (uint8_t)packet->_snr;            // inbound SNR ×4, for the requester
      memcpy(&resp[2], &tag, 4);                  // echo the tag so requester can match
      memcpy(&resp[6], rooms[0].id.pub_key, PUB_KEY_SIZE);
      auto r = createControlData(resp, sizeof(resp));
      // widened random delay ×4 — many nodes may answer the same request.
      if (r) sendZeroHop(r, getRetransmitDelay(r) * 4);
    }

  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP &&
             packet->payload_len >= 6 + PUB_KEY_SIZE) {
    if (_pending_discover_tag == 0 || millisHasNowPassed(_pending_discover_until)) {
      _pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != _pending_discover_tag) return;
    uint8_t node_type = packet->payload[0] & 0x0F;
    mesh::Identity id(&packet->payload[6]);
    if (id.matches(rooms[0].id)) return;  // skip ourselves
    putNeighbour(id, getRTCClock()->getCurrentTime(), packet->getSNR(), node_type);
  }
}

/**
 * Store a name→pubkey-prefix mapping in the name table (JES-840).
 * Used by handleSyncDat to learn author names from peer SYNCDAT frames.
 * pub4 = first NAME_KEY_SIZE bytes of the author's pubkey.
 */
void MultiRoomMesh::storeName(const uint8_t* pub4, const char* name) {
  if (!name || name[0] == '\0') return;
  // Find existing entry or LRU victim. Manual entries (JES-875) are pinned:
  // never overwritten by a learned name, never evicted.
  int victim = -1;
  uint32_t min_seq = UINT32_MAX;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq == 0) {
      victim = i;
      break;
    }
    if (memcmp(_names[i].pub_prefix, pub4, NAME_KEY_SIZE) == 0) {
      if (_names[i].manual) return;   // operator-set name wins
      StrHelper::strncpy(_names[i].name, name, sizeof(_names[i].name));
      _names[i].lru_seq = ++_name_lru_ctr;
      saveNameTable();
      return;
    }
    if (!_names[i].manual && _names[i].lru_seq < min_seq) {
      min_seq = _names[i].lru_seq; victim = i;
    }
  }
  if (victim < 0) return;  // table full of manual entries
  memcpy(_names[victim].pub_prefix, pub4, NAME_KEY_SIZE);
  StrHelper::strncpy(_names[victim].name, name, sizeof(_names[victim].name));
  _names[victim].lru_seq = ++_name_lru_ctr;
  _names[victim].manual = false;
  saveNameTable();
}

/* ------------------------------------------------------------------ */
/*  Manual name-table management (JES-875)                              */
/* ------------------------------------------------------------------ */
bool MultiRoomMesh::setNameManual(const uint8_t* pub4, const char* name) {
  if (!pub4 || !name || name[0] == '\0') return false;
  int empty = -1;         // first empty slot
  int lru_victim = -1;    // lowest-lru non-manual slot (fallback)
  uint32_t min_seq = UINT32_MAX;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq == 0) { if (empty < 0) empty = i; continue; }
    if (memcmp(_names[i].pub_prefix, pub4, NAME_KEY_SIZE) == 0) {
      // Update existing entry, promote to manual (pinned).
      StrHelper::strncpy(_names[i].name, name, sizeof(_names[i].name));
      _names[i].lru_seq = ++_name_lru_ctr;
      _names[i].manual = true;
      saveNameTable();
      return true;
    }
    if (!_names[i].manual && _names[i].lru_seq < min_seq) {
      min_seq = _names[i].lru_seq; lru_victim = i;
    }
  }
  int victim = (empty >= 0) ? empty : lru_victim;
  if (victim < 0) return false;  // table full of manual entries
  memcpy(_names[victim].pub_prefix, pub4, NAME_KEY_SIZE);
  StrHelper::strncpy(_names[victim].name, name, sizeof(_names[victim].name));
  _names[victim].lru_seq = ++_name_lru_ctr;
  _names[victim].manual = true;
  saveNameTable();
  return true;
}

bool MultiRoomMesh::delName(const uint8_t* pub4) {
  if (!pub4) return false;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq != 0 &&
        memcmp(_names[i].pub_prefix, pub4, NAME_KEY_SIZE) == 0) {
      memset(&_names[i], 0, sizeof(NameEntry));  // lru_seq=0 marks empty
      saveNameTable();
      return true;
    }
  }
  return false;
}

void MultiRoomMesh::addServerPost(int room_idx, const char* text) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || !rooms[room_idx].active || room_idx == 0) return;  // JES-846
  if (!text || text[0] == 0) return;

  // Build a transient ClientInfo whose identity = the room itself
  ClientInfo server_ci;
  memset(&server_ci, 0, sizeof(server_ci));
  memcpy(server_ci.id.pub_key, rooms[room_idx].id.pub_key, PUB_KEY_SIZE);
  server_ci.permissions = PERM_ACL_ADMIN;

  // Label as operator post
  char labeled[MAX_POST_TEXT_LEN + 1];
  snprintf(labeled, sizeof(labeled), "[OP] %s", text);

  addPost(rooms[room_idx], &server_ci, labeled);
}

String MultiRoomMesh::buildNickJson(int room_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || !rooms[room_idx].active) return "[]";

  String json = "[";
  bool first = true;
  int nc = rooms[room_idx].acl.getNumClients();
  for (int i = 0; i < nc; i++) {
    ClientInfo* ci = rooms[room_idx].acl.getClientByIdx(i);
    if (!ci || ci->permissions == 0) continue;
    if (!first) json += ",";
    first = false;
    uint8_t role = ci->permissions & PERM_ACL_ROLE_MASK;
    // pubkey hex prefix (4 bytes = 8 hex chars) — used by DM feature
    static const char hxn[] = "0123456789abcdef";
    json += "{\"pub\":\"";
    for (int b = 0; b < NAME_KEY_SIZE; b++) {
      json += hxn[ci->id.pub_key[b] >> 4];
      json += hxn[ci->id.pub_key[b] & 0x0f];
    }
    json += "\",\"name\":\"";
    // Escape name characters that could break JSON (names come from advert parser)
    const char* nm = resolveName(ci->id.pub_key);
    for (const char* c = nm; *c; c++) {
      if (*c == '"' || *c == '\\') json += '\\';
      json += *c;
    }
    json += "\",\"role\":";
    json += (int)role;
    json += ",\"last\":";
    json += (unsigned long)ci->last_activity;
    json += "}";
  }
  json += "]";
  return json;
}

/* ------------------------------------------------------------------ */
/*  DM ring buffer (JES-808)                                           */
/* ------------------------------------------------------------------ */

static inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
  return 0;
}

void MultiRoomMesh::dmBuffer(const uint8_t* pub_prefix, uint32_t ts,
                             bool outgoing, const char* text) {
  int ci = -1;
  for (int i = 0; i < _dm_num_convs; i++) {
    if (memcmp(_dm_convs[i].pub_prefix, pub_prefix, NAME_KEY_SIZE) == 0) {
      ci = i; break;
    }
  }
  if (ci < 0) {
    if (_dm_num_convs >= DM_MAX_CONVS) return;  // table full — drop silently
    ci = _dm_num_convs++;
    memcpy(_dm_convs[ci].pub_prefix, pub_prefix, NAME_KEY_SIZE);
    _dm_convs[ci].head  = 0;
    _dm_convs[ci].count = 0;
  }
  DmConv& c = _dm_convs[ci];
  DmMsg&  m = c.msgs[c.head];
  m.ts       = ts;
  m.outgoing = outgoing;
  int tlen = strlen(text);
  if (tlen >= DM_TEXT_LEN) tlen = DM_TEXT_LEN - 1;
  memcpy(m.text, text, tlen);
  m.text[tlen] = 0;
  c.head = (c.head + 1) % DM_MAX_MSGS;
  if (c.count < DM_MAX_MSGS) c.count++;
}

bool MultiRoomMesh::dmSend(const char* pub_hex, const char* text) {
  if (!pub_hex || strlen(pub_hex) < NAME_KEY_SIZE * 2) return false;
  if (!text || text[0] == 0) return false;

  uint8_t prefix[NAME_KEY_SIZE];
  for (int i = 0; i < NAME_KEY_SIZE; i++) {
    prefix[i] = (uint8_t)((hexNibble(pub_hex[i * 2]) << 4) | hexNibble(pub_hex[i * 2 + 1]));
  }

  for (int r = 0; r < MAX_ROOMS; r++) {
    if (!rooms[r].active) continue;
    int nc = rooms[r].acl.getNumClients();
    for (int i = 0; i < nc; i++) {
      ClientInfo* ci = rooms[r].acl.getClientByIdx(i);
      if (!ci || ci->permissions == 0) continue;
      if (memcmp(ci->id.pub_key, prefix, NAME_KEY_SIZE) != 0) continue;

      uint32_t now = getRTCClock()->getCurrentTimeUnique();
      int tlen = strlen(text);
      if (tlen > MAX_POST_TEXT_LEN) tlen = MAX_POST_TEXT_LEN;

      int dlen = 0;
      memcpy(&reply_data[dlen], &now, 4); dlen += 4;
      reply_data[dlen++] = (TXT_TYPE_PLAIN << 2);
      memcpy(&reply_data[dlen], text, tlen); dlen += tlen;

      self_id = rooms[r].id;
      auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, ci->id, ci->shared_secret,
                                reply_data, dlen);
      if (!pkt) return false;

      if (ci->out_path_len == OUT_PATH_UNKNOWN) {
        sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
      } else {
        sendDirect(pkt, ci->out_path, ci->out_path_len, SERVER_RESPONSE_DELAY);
      }
      dmBuffer(prefix, now, true, text);
      return true;
    }
  }
  return false;  // client not found in any room
}

String MultiRoomMesh::buildDmConvsJson() {
  static const char hx[] = "0123456789abcdef";
  String j = "[";
  bool first = true;
  for (int i = 0; i < _dm_num_convs; i++) {
    DmConv& c = _dm_convs[i];
    if (c.count == 0) continue;
    if (!first) j += ",";
    first = false;
    uint32_t last_ts = 0;
    for (int m = 0; m < c.count; m++)
      if (c.msgs[m].ts > last_ts) last_ts = c.msgs[m].ts;
    j += "{\"pub\":\"";
    for (int b = 0; b < NAME_KEY_SIZE; b++) {
      j += hx[c.pub_prefix[b] >> 4];
      j += hx[c.pub_prefix[b] & 0x0f];
    }
    j += "\",\"name\":\"";
    const char* nm = resolveName(c.pub_prefix);
    for (const char* p = nm; *p; p++) {
      if (*p == '"' || *p == '\\') j += '\\';
      j += *p;
    }
    j += "\",\"last\":"; j += (unsigned long)last_ts; j += "}";
  }
  j += "]";
  return j;
}

String MultiRoomMesh::buildDmThreadJson(const char* pub_hex) {
  if (!pub_hex || strlen(pub_hex) < NAME_KEY_SIZE * 2) return "[]";
  uint8_t prefix[NAME_KEY_SIZE];
  for (int i = 0; i < NAME_KEY_SIZE; i++) {
    prefix[i] = (uint8_t)((hexNibble(pub_hex[i * 2]) << 4) | hexNibble(pub_hex[i * 2 + 1]));
  }
  int ci = -1;
  for (int i = 0; i < _dm_num_convs; i++) {
    if (memcmp(_dm_convs[i].pub_prefix, prefix, NAME_KEY_SIZE) == 0) {
      ci = i; break;
    }
  }
  if (ci < 0) return "[]";

  DmConv& c = _dm_convs[ci];
  // Walk ring from oldest to newest
  int start = (c.head - c.count + DM_MAX_MSGS * 2) % DM_MAX_MSGS;
  String j = "[";
  bool first = true;
  for (int i = 0; i < c.count; i++) {
    int idx = (start + i) % DM_MAX_MSGS;
    DmMsg& m = c.msgs[idx];
    if (!first) j += ",";
    first = false;
    j += "{\"ts\":"; j += (unsigned long)m.ts;
    j += ",\"out\":"; j += m.outgoing ? "1" : "0";
    j += ",\"text\":\"";
    for (const char* p = m.text; *p; p++) {
      if (*p == '"')  j += "\\\"";
      else if (*p == '\\') j += "\\\\";
      else if (*p == '\n') j += "\\n";
      else if ((unsigned char)*p >= 0x20) j += *p;
    }
    j += "\"}";
  }
  j += "]";
  return j;
}

/* ------------------------------------------------------------------ */
/*  Post pool backup / restore (JES-790)                               */
/* ------------------------------------------------------------------ */

/** Extract a JSON quoted-string value, handling \" and \\ escapes.    */
static bool extractJsonString(const String& json, const char* key,
                              char* dest, size_t dlen) {
  String k = String("\"") + key + "\":\"";
  int pos = json.indexOf(k);
  if (pos < 0) return false;
  pos += k.length();

  size_t out = 0;
  while (pos < (int)json.length() && out < dlen - 1) {
    char c = json[pos];
    if (c == '\\' && pos + 1 < (int)json.length()) {
      char n = json[pos + 1];
      if      (n == '"')  { dest[out++] = '"';  pos += 2; }
      else if (n == '\\') { dest[out++] = '\\'; pos += 2; }
      else                { dest[out++] = n;    pos += 2; }
    } else if (c == '"') {
      break;
    } else {
      dest[out++] = c;
      pos++;
    }
  }
  dest[out] = '\0';
  return true;
}

String MultiRoomMesh::getPostsFlatJson() const {
  char hex[PUB_KEY_SIZE * 2 + 1];
  String j;
  int count = 0;

  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    const PostInfo& p = _post_pool[i];
    if (p.room_idx == 0xFF) continue;

    char pfx[16];
    snprintf(pfx, sizeof(pfx), "post%d_", count);

    // room index, timestamp (stored as quoted strings for uniform parsing)
    j += "\""; j += pfx; j += "ri\":\""; j += (int)p.room_idx;    j += "\",";
    j += "\""; j += pfx; j += "ts\":\""; j += p.post_timestamp;   j += "\",";

    // author public key (hex)
    mesh::Utils::toHex(hex, p.author.pub_key, PUB_KEY_SIZE);
    j += "\""; j += pfx; j += "ak\":\""; j += hex; j += "\",";

    // text (escape " and \)
    j += "\""; j += pfx; j += "tx\":\"";
    for (const char* c = p.text; *c; c++) {
      if      (*c == '"')  { j += "\\\""; }
      else if (*c == '\\') { j += "\\\\"; }
      else                 { j += *c; }
    }
    j += "\",";

    count++;
  }

  j += "\"post_count\":\""; j += count; j += "\"";
  return j;
}

bool MultiRoomMesh::restorePostsFlatJson(const String& json) {
  char buf[12];
  if (!extractJsonString(json, "post_count", buf, sizeof(buf))) return false;
  int count = atoi(buf);
  if (count <= 0) return true;

  // Clear pool and per-room post counts
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    memset(&_post_pool[i], 0, sizeof(PostInfo));
    _post_pool[i].room_idx = 0xFF;
  }
  for (int i = 0; i < MAX_ROOMS; i++) rooms[i].num_posted = 0;

  char ri_str[4], ts_str[12], ak_hex[PUB_KEY_SIZE * 2 + 2];
  char tx_str[MAX_POST_TEXT_LEN + 2];
  char ri_k[24], ts_k[24], ak_k[24], tx_k[24];
  int pool_idx = 0;

  for (int n = 0; n < count && pool_idx < MAX_TOTAL_POSTS; n++) {
    snprintf(ri_k, sizeof(ri_k), "post%d_ri", n);
    snprintf(ts_k, sizeof(ts_k), "post%d_ts", n);
    snprintf(ak_k, sizeof(ak_k), "post%d_ak", n);
    snprintf(tx_k, sizeof(tx_k), "post%d_tx", n);

    if (!extractJsonString(json, ri_k, ri_str, sizeof(ri_str))) continue;
    if (!extractJsonString(json, ts_k, ts_str, sizeof(ts_str))) continue;
    if (!extractJsonString(json, ak_k, ak_hex, sizeof(ak_hex))) continue;
    extractJsonString(json, tx_k, tx_str, sizeof(tx_str));  // empty text OK

    int ri = atoi(ri_str);
    if (ri < 0 || ri >= MAX_ROOMS || !rooms[ri].active) continue;

    PostInfo& entry = _post_pool[pool_idx];
    if (!mesh::Utils::fromHex(entry.author.pub_key, PUB_KEY_SIZE, ak_hex)) continue;
    entry.post_timestamp = (uint32_t)strtoul(ts_str, nullptr, 10);
    strncpy(entry.text, tx_str, MAX_POST_TEXT_LEN);
    entry.text[MAX_POST_TEXT_LEN] = '\0';
    entry.room_idx = (uint8_t)ri;
    rooms[ri].num_posted++;
    pool_idx++;
  }

  savePostPool();
  return true;
}

/* ------------------------------------------------------------------ */
/*  Phase 5: anti-entropy replication                                   */
/* ------------------------------------------------------------------ */

/* Compute and cache ECDH(rooms[0].priv, peer.pub) for peer pi. */
void MultiRoomMesh::calcPeerSecret(int pi) {
  if (peers[pi].secret_valid) return;
  rooms[0].id.calcSharedSecret(peers[pi].shared_secret, peers[pi].pub_key);
  peers[pi].secret_valid = true;
}

/**
 * Update the version vector for room slot: record that origin has posts
 * up to timestamp ts.  Creates a new entry or evicts the oldest if full.
 * Returns true if the slot was updated.
 */
bool MultiRoomMesh::vvUpdate(RoomSlot& slot, const uint8_t* origin_id, uint32_t ts) {
  // Find existing entry
  for (int i = 0; i < MAX_VV_ORIGINS; i++) {
    if (memcmp(slot.vv[i].origin_id, origin_id, 4) == 0 && slot.vv[i].seq != 0) {
      if (ts > slot.vv[i].seq) slot.vv[i].seq = ts;
      return true;
    }
  }
  // New origin: use empty slot
  for (int i = 0; i < MAX_VV_ORIGINS; i++) {
    if (slot.vv[i].seq == 0) {
      memcpy(slot.vv[i].origin_id, origin_id, 4);
      slot.vv[i].seq = ts;
      return true;
    }
  }
  // VV full: evict the entry with the oldest seq (least recently updated)
  VVEntry* oldest = &slot.vv[0];
  for (int i = 1; i < MAX_VV_ORIGINS; i++) {
    if (slot.vv[i].seq < oldest->seq) oldest = &slot.vv[i];
  }
  memcpy(oldest->origin_id, origin_id, 4);
  oldest->seq = ts;
  return true;
}

/**
 * Ingest one post received via SYNCDAT/push from a peer.
 * Deduplicates by (origin_id, post_timestamp).  Stores in ridx's pool.
 * Returns true if the post was new and ingested.
 */
bool MultiRoomMesh::ingestSyncPost(uint8_t ridx, const uint8_t* origin_id,
                                    uint32_t ts, const uint8_t* author_pub,
                                    const char* text, uint32_t msg_id) {
  if (ridx >= MAX_ROOMS || !rooms[ridx].active) return false;

  // Resurrection guard: silently drop tombstoned posts (JES-824)
  if (isTombstoned(origin_id, ts)) return false;

  // Dedup check: already have this (origin, ts) pair?
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    if (_post_pool[i].room_idx == ridx &&
        _post_pool[i].post_timestamp == ts &&
        memcmp(_post_pool[i].origin_id, origin_id, 4) == 0) {
      return false;   // duplicate
    }
  }

  // JES-861 echo suppression: a client message flooded to BOTH coupled
  // room-servers is ingested independently by each, producing two posts that
  // share the same logical identity (author, msg_id) but carry DIFFERENT
  // origin_id/post_timestamp — so the (origin, ts) dedup above cannot collapse
  // them and the user sees every message twice ~1 s apart. If we already hold
  // this logical message, drop the peer's redundant copy but still advance the
  // VV for its origin so the peer stops resending it every sync round.
  // msg_id == 0 (server/OP posts, pre-JES-861 peers) falls back to (origin,ts)
  // dedup only, which never collapses distinct messages -> no message loss.
  if (msg_id != 0) {
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (_post_pool[i].room_idx == ridx &&
          _post_pool[i].msg_id == msg_id &&
          memcmp(_post_pool[i].author.pub_key, author_pub, 4) == 0) {
        vvUpdate(rooms[ridx], origin_id, ts);   // mark known -> peer stops resending
        return false;   // echo of a message we already have
      }
    }
  }

  RoomSlot& slot = rooms[ridx];
  int quota = MAX_TOTAL_POSTS / (_num_active_rooms > 0 ? _num_active_rooms : 1);

  // Find a free slot; track oldest for this room
  PostInfo* free_slot = nullptr;
  PostInfo* oldest_for_room = nullptr;
  int room_count = 0;
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    PostInfo& p = _post_pool[i];
    if (p.room_idx == 0xFF) {
      if (!free_slot) free_slot = &p;
    } else if (p.room_idx == ridx) {
      room_count++;
      if (!oldest_for_room || p.post_timestamp < oldest_for_room->post_timestamp)
        oldest_for_room = &p;
    }
  }
  if (room_count >= quota && oldest_for_room) {
    memset(oldest_for_room, 0, sizeof(PostInfo));
    oldest_for_room->room_idx = 0xFF;
    if (!free_slot) free_slot = oldest_for_room;
  }
  if (!free_slot) {
    // Pool full globally — evict oldest
    PostInfo* oldest_global = nullptr;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (!oldest_global || _post_pool[i].post_timestamp < oldest_global->post_timestamp)
        oldest_global = &_post_pool[i];
    }
    memset(oldest_global, 0, sizeof(PostInfo));
    oldest_global->room_idx = 0xFF;
    free_slot = oldest_global;
  }

  memcpy(free_slot->author.pub_key, author_pub, 4);  // 4-byte prefix
  StrHelper::strncpy(free_slot->text, text, MAX_POST_TEXT_LEN);
  free_slot->post_timestamp = ts;
  free_slot->msg_id         = msg_id;   // JES-861: cross-node message identity
  free_slot->room_idx       = ridx;
  memcpy(free_slot->origin_id, origin_id, 4);

  slot.num_posted++;
  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _post_dirty_at = futureMillis(5000);

  // Update VV
  vvUpdate(slot, origin_id, ts);

  // Notify MQTT transport (same as addPost) — publish replicated posts too
  if (_mqtt_post_cb) {
    _mqtt_post_cb((int)ridx, ts, author_pub, text, _mqtt_post_ctx);
  }

  return true;
}

/**
 * Push a single post to peer pi via SYNCDAT DM.
 * Wire format (JES-816 multi-room, JES-840):
 *   [4:ts][1:flags][4:room_hash][4:post_ts][4:origin_id][4:author_pub][1:name_len][name][text]
 * room_hash = first 4 bytes of the sending room's public key.
 * name_len = 0..23; name = advertised author name for receiving node's name table.
 * All sync DMs are sent from rooms[0].id (node transport identity).
 */
void MultiRoomMesh::pushPostToPeer(int pi, RoomSlot& slot, PostInfo& post) {
  if (post.room_idx == 0) return;  // JES-846: room 0 is identity-only, never sync its posts
  DLOG(post.post_timestamp,
       "SYNC DAT→peer[%d] room=%d ts=%lu orig=%02x%02x%02x%02x",
       pi, (int)post.room_idx, (unsigned long)post.post_timestamp,
       (unsigned)post.origin_id[0], (unsigned)post.origin_id[1],
       (unsigned)post.origin_id[2], (unsigned)post.origin_id[3]);
  calcPeerSecret(pi);

  int len = 0;
  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(&reply_data[len], &now, 4);                        len += 4;
  reply_data[len++] = (TXT_TYPE_SYNCDAT2 << 2);            // JES-861: msg_id-bearing frame
  memcpy(&reply_data[len], slot.id.pub_key, 4);             len += 4;  // room_hash
  memcpy(&reply_data[len], &post.post_timestamp, 4);        len += 4;
  memcpy(&reply_data[len], post.origin_id, 4);              len += 4;
  memcpy(&reply_data[len], post.author.pub_key, 4);         len += 4;
  memcpy(&reply_data[len], &post.msg_id, 4);                len += 4;  // JES-861: cross-node message id
  // Bug 2 (JES-840): embed author name so the receiving node can populate its name table
  const char* aname = resolveName(post.author.pub_key);
  uint8_t name_len = (uint8_t)strlen(aname);
  if (name_len > 23) name_len = 23;
  reply_data[len++] = name_len;
  memcpy(&reply_data[len], aname, name_len);                len += name_len;
  // clamp text to remaining space
  int max_text = (int)sizeof(reply_data) - len;
  if (max_text < 0) max_text = 0;
  int text_len = strlen(post.text);
  if (text_len > max_text) text_len = max_text;
  memcpy(&reply_data[len], post.text, text_len);            len += text_len;

  // Send from node transport identity (rooms[0])
  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  self_id = rooms[0].id;
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                             peers[pi].shared_secret, reply_data, len);
  if (pkt) {
    sendFlood(pkt, (uint32_t)0, (uint8_t)(_prefs.path_hash_mode + 1));
    peers[pi].sync_posts_sent++;
    _sync_posts_sent++;
  } else {
    // Bug 1B (JES-840): log push failure so it is visible on serial
    Serial.printf("[SYNC] pushPostToPeer: createDatagram FAILED for peer[%d] '%s' (secret_valid=%d)\n",
                  pi, peers[pi].name, (int)peers[pi].secret_valid);
  }
}

/**
 * Send SYNCREQ to peer pi for each active room (JES-816 multi-room).
 * Wire format per room:
 *   [4:ts][1:flags][4:room_hash][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 * room_hash = first 4 bytes of the room's public key.
 * One SYNCREQ is sent per active room, staggered by 1 s to respect 1% duty cycle.
 * All packets use rooms[0].id as sender (node transport identity).
 */
void MultiRoomMesh::sendSyncReq(int pi, bool full) {
  if (!peers[pi].active) return;
  calcPeerSecret(pi);

  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  uint32_t stagger_ms = 0;
  int rooms_synced = 0;

  for (int ri = 1; ri < MAX_ROOMS; ri++) {  // JES-846: start at 1, room 0 is identity-only
    if (!rooms[ri].active) continue;
    RoomSlot& slot = rooms[ri];

    int len = 0;
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    memcpy(&reply_data[len], &now, 4);               len += 4;
    reply_data[len++] = (TXT_TYPE_SYNCREQ << 2);
    memcpy(&reply_data[len], slot.id.pub_key, 4);    len += 4;  // room_hash

    uint8_t num_vv = 0;
    int vv_count_pos = len;
    reply_data[len++] = 0;   // placeholder for num_vv

    // JES-874: a full resync advertises an EMPTY version vector so the peer treats
    // us as knowing nothing and resends every post it has (dedup by (origin_id,ts)
    // fills any gaps). This recovers pre-upgrade posts stranded under the old shared
    // room-key origin whose high-watermark blocked normal incremental pulls.
    if (!full) {
      for (int i = 0; i < MAX_VV_ORIGINS && len + 8 <= (int)sizeof(reply_data); i++) {
        if (slot.vv[i].seq == 0) continue;
        memcpy(&reply_data[len], slot.vv[i].origin_id, 4); len += 4;
        memcpy(&reply_data[len], &slot.vv[i].seq,      4); len += 4;
        num_vv++;
      }
    }
    reply_data[vv_count_pos] = num_vv;

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, reply_data, len);
    if (pkt) {
      sendFlood(pkt, stagger_ms, (uint8_t)(_prefs.path_hash_mode + 1));
      stagger_ms += 1000;
      rooms_synced++;
    }
  }

  Serial.printf("[SYNC] SYNCREQ → peer[%d] '%s' (%d room(s))\n",
                pi, peers[pi].name, rooms_synced);
  peers[pi].last_syncreq_ts = getRTCClock()->getCurrentTime();
  DLOG(peers[pi].last_syncreq_ts,
       "SYNC REQ→peer[%d] '%s' rooms=%d",
       pi, peers[pi].name, rooms_synced);
  _sync_req_sent++;
}

/**
 * Handle incoming SYNCREQ from peer pi (JES-816 multi-room).
 * Wire format: [4:ts][1:flags][4:room_hash][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 * room_hash identifies which room's posts the peer wants.
 * We find the matching local room, send SYNCDAT for missing posts, then SYNCEND.
 */
void MultiRoomMesh::handleSyncReq(int pi, uint8_t* data, size_t len) {
  if (len < 10) return;  // [4:ts][1:flags][4:room_hash][1:num_vv] min
  uint8_t room_hash[4];
  memcpy(room_hash, &data[5], 4);
  uint8_t num_vv = data[9];
  if ((size_t)(10 + (int)num_vv * 8) > len) return;
  DLOG((uint32_t)(millis()/1000),
       "SYNC REQ←peer[%d] hash=%02x%02x%02x%02x vv=%d",
       pi,
       (unsigned)room_hash[0], (unsigned)room_hash[1],
       (unsigned)room_hash[2], (unsigned)room_hash[3],
       (int)num_vv);

  // Find local room matching hash (first 4 bytes of pub_key); skip room 0 (JES-846)
  int ri = -1;
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (rooms[i].active && memcmp(rooms[i].id.pub_key, room_hash, 4) == 0) {
      ri = i; break;
    }
  }
  // Fallback: if no exact hash match, sync into first active room (skip room 0).
  // This allows cross-node sync when both nodes have different room keys (JES-723).
  // The SYNCDAT will echo back the requesting room_hash, so the peer can ingest correctly.
  if (ri < 0) {
    for (int i = 1; i < MAX_ROOMS; i++) {
      if (rooms[i].active) { ri = i; break; }
    }
  }
  if (ri < 0) return;  // no active rooms at all

  // Parse peer's VV
  struct { uint8_t orig[4]; uint32_t seq; } peer_vv[MAX_VV_ORIGINS];
  uint8_t peer_vv_count = (num_vv > MAX_VV_ORIGINS) ? MAX_VV_ORIGINS : num_vv;
  for (int i = 0; i < peer_vv_count; i++) {
    memcpy(peer_vv[i].orig, &data[10 + i * 8],     4);
    memcpy(&peer_vv[i].seq,  &data[10 + i * 8 + 4], 4);
  }

  auto peerKnows = [&](const uint8_t* orig) -> uint32_t {
    for (int i = 0; i < peer_vv_count; i++) {
      if (memcmp(peer_vv[i].orig, orig, 4) == 0) return peer_vv[i].seq;
    }
    return 0;
  };

  calcPeerSecret(pi);
  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  // Send SYNCDAT for each post in room ri that the peer is missing
  int sent = 0;
  uint32_t delay_ms = 500;
  for (int k = 0; k < MAX_TOTAL_POSTS && sent < MAX_SYNC_POSTS; k++) {
    const PostInfo& p = _post_pool[k];
    if (p.room_idx != (uint8_t)ri) continue;
    uint32_t peer_knows = peerKnows(p.origin_id);
    if (p.post_timestamp <= peer_knows) continue;
    if (isTombstoned(p.origin_id, p.post_timestamp)) continue;  // don't push deleted posts (JES-824)

    int dlen = 0;
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    uint8_t buf[MAX_PACKET_PAYLOAD];
    memcpy(&buf[dlen], &now, 4);               dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCDAT << 2);
    memcpy(&buf[dlen], room_hash, 4);          dlen += 4;  // room_hash
    memcpy(&buf[dlen], &p.post_timestamp, 4);  dlen += 4;
    memcpy(&buf[dlen], p.origin_id, 4);        dlen += 4;
    memcpy(&buf[dlen], p.author.pub_key, 4);   dlen += 4;
    // Bug 2 (JES-840): embed author name so receiving node can populate its name table
    const char* pname = resolveName(p.author.pub_key);
    uint8_t pname_len = (uint8_t)strlen(pname);
    if (pname_len > 23) pname_len = 23;
    buf[dlen++] = pname_len;
    memcpy(&buf[dlen], pname, pname_len);      dlen += pname_len;
    // clamp text to remaining space
    int max_tlen = (int)sizeof(buf) - dlen;
    if (max_tlen < 0) max_tlen = 0;
    int tlen = strlen(p.text);
    if (tlen > max_tlen) tlen = max_tlen;
    memcpy(&buf[dlen], p.text, tlen);          dlen += tlen;

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) {
      sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
      delay_ms += 500;
      sent++;
    }
  }

  // Send SYNCEND with our VV for this room
  {
    int dlen = 0;
    uint8_t buf[MAX_PACKET_PAYLOAD];
    uint32_t now2 = getRTCClock()->getCurrentTimeUnique();
    memcpy(&buf[dlen], &now2, 4);   dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCEND << 2);
    memcpy(&buf[dlen], room_hash, 4); dlen += 4;  // room_hash
    uint8_t nvv = 0;
    int nvv_pos = dlen++;
    RoomSlot& rslot = rooms[ri];
    for (int i = 0; i < MAX_VV_ORIGINS && dlen + 8 <= (int)sizeof(buf); i++) {
      if (rslot.vv[i].seq == 0) continue;
      memcpy(&buf[dlen], rslot.vv[i].origin_id, 4); dlen += 4;
      memcpy(&buf[dlen], &rslot.vv[i].seq,      4); dlen += 4;
      nvv++;
    }
    buf[nvv_pos] = nvv;
    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
  }

  // Relay all tombstones to peer (idempotent; bounded by MAX_TOMBSTONES=64) (JES-824)
  // We send them after SYNCEND so the peer can immediately apply them.
  for (uint8_t ti = 0; ti < _tombstone_count; ti++) {
    uint8_t dbuf[MAX_PACKET_PAYLOAD];
    int ddlen = 0;
    uint32_t tnow = getRTCClock()->getCurrentTimeUnique();
    memcpy(&dbuf[ddlen], &tnow, 4);                                  ddlen += 4;
    dbuf[ddlen++] = (TXT_TYPE_SYNCDEL << 2);
    memcpy(&dbuf[ddlen], _tombstones[ti].room_hash, 4);              ddlen += 4;
    memcpy(&dbuf[ddlen], _tombstones[ti].origin_id, 4);              ddlen += 4;
    memcpy(&dbuf[ddlen], &_tombstones[ti].post_ts,  4);              ddlen += 4;
    self_id = rooms[0].id;
    auto tpkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                                peers[pi].shared_secret, dbuf, ddlen);
    if (tpkt) {
      sendFlood(tpkt, delay_ms, _prefs.path_hash_mode + 1);
      delay_ms += 200;
    }
  }

  Serial.printf("[SYNC] SYNCREQ from peer[%d]: sent %d post(s) for room[%d]\n", pi, sent, ri);
}

/**
 * Handle incoming SYNCDAT from peer pi — ingest one post (JES-816 multi-room, JES-840).
 * Wire format: [4:ts][1:flags][4:room_hash][4:post_ts][4:origin_id][4:author_pub][1:name_len][name][text]
 * name_len=0 means no name embedded (hex fallback on display).
 * Routes the post to the local room matching room_hash.
 */
void MultiRoomMesh::handleSyncDat(int pi, uint8_t* data, size_t len) {
  // JES-861: SYNCDAT2 (flag 9) inserts a 4-byte msg_id after author_pub;
  // legacy SYNCDAT (flag 5) has none. Parse the correct layout for each so a
  // node still talking the old format (e.g. mid-OTA) is handled without garbage.
  bool     has_msgid = ((data[4] >> 2) == TXT_TYPE_SYNCDAT2);
  size_t   min_hdr   = has_msgid ? 26 : 22;
  if (len < min_hdr) return;  // [4:ts][1:flags][4:room_hash][4:post_ts][4:origin_id][4:author_pub](+[4:msg_id])[1:name_len]
  uint8_t  room_hash[4];
  uint32_t post_ts;
  uint8_t  origin_id[4], author_pub[4];
  uint32_t msg_id = 0;
  memcpy(room_hash,  &data[5],  4);
  memcpy(&post_ts,   &data[9],  4);
  memcpy(origin_id,  &data[13], 4);
  memcpy(author_pub, &data[17], 4);
  size_t nl_pos = 21;
  if (has_msgid) { memcpy(&msg_id, &data[21], 4); nl_pos = 25; }
  // Bug 2 (JES-840): parse embedded author name and store in name table
  uint8_t name_len = data[nl_pos];
  if (name_len > 23) name_len = 23;  // clamp — reject oversized (no buffer overflow)
  if ((size_t)(nl_pos + 1 + name_len) > len) return;  // bounds guard for name + text
  if (name_len > 0) {
    char author_name[24];
    memcpy(author_name, &data[nl_pos + 1], name_len);
    author_name[name_len] = '\0';
    storeName(author_pub, author_name);
  }
  data[len] = 0;   // NUL-terminate text
  const char* text = (const char*)&data[nl_pos + 1 + name_len];

  // Find local room matching hash; skip room 0 (JES-846)
  int ri = -1;
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (rooms[i].active && memcmp(rooms[i].id.pub_key, room_hash, 4) == 0) {
      ri = i; break;
    }
  }
  // Fallback: accept push from authenticated peer even when room_hash doesn't match.
  // Occurs when nodes have different room keys (normal case: each node has its own
  // random identity). Packet is already ECDH-verified by onPeerDataRecv. (JES-835)
  if (ri < 0) {
    for (int i = 1; i < MAX_ROOMS; i++) {
      if (rooms[i].active) { ri = i; break; }
    }
  }
  if (ri < 0) return;  // no active rooms

  peers[pi].last_syncdat_ts = getRTCClock()->getCurrentTime();
  _sync_dat_recv++;

  bool added = ingestSyncPost((uint8_t)ri, origin_id, post_ts, author_pub, text, msg_id);
  DLOG(post_ts,
       "SYNC DAT←peer[%d] orig=%02x%02x%02x%02x ts=%lu room=%d %s",
       pi,
       (unsigned)origin_id[0], (unsigned)origin_id[1],
       (unsigned)origin_id[2], (unsigned)origin_id[3],
       (unsigned long)post_ts, ri, added ? "added" : "dup/tomb");
  if (added) {
    Serial.printf("[SYNC] SYNCDAT from peer[%d]: +post ts=%lu → room[%d]\n",
                  pi, (unsigned long)post_ts, ri);
    peers[pi].sync_posts_recv++;
    _sync_posts_recv++;
  }
}

/**
 * Handle incoming SYNCEND from peer pi (JES-816 multi-room).
 * Wire format: [4:ts][1:flags][4:room_hash][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 *
 * Bidirectionality (JES-841 fix): the SYNCEND carries the peer's VV.  We use
 * that VV to immediately push back any local posts the peer is missing.  This
 * completes a full two-way exchange in one round-trip WITHOUT resetting the
 * periodic timer to 2 s (which caused a tight A→B pull loop that starved the
 * independent B→A pull direction and exhausted LoRa duty-cycle).
 *
 * We do NOT send a reverse SYNCEND here to avoid an infinite ping-pong:
 * the next periodic SYNCREQ from either side will carry the updated VV.
 */
void MultiRoomMesh::handleSyncEnd(int pi, uint8_t* data, size_t len) {
  if (len < 10) return;  // [4:ts][1:flags][4:room_hash][1:num_vv]

  uint8_t room_hash[4];
  memcpy(room_hash, &data[5], 4);
  uint8_t num_vv = data[9];
  if (num_vv > MAX_VV_ORIGINS) num_vv = MAX_VV_ORIGINS;
  // Bounds-check: each VV entry is 8 bytes; truncate if frame is too short.
  while (num_vv > 0 && (size_t)(10 + (int)num_vv * 8) > len) num_vv--;
  DLOG((uint32_t)(millis()/1000),
       "SYNC END←peer[%d] hash=%02x%02x%02x%02x vv=%d",
       pi,
       (unsigned)room_hash[0], (unsigned)room_hash[1],
       (unsigned)room_hash[2], (unsigned)room_hash[3],
       (int)num_vv);

  // Find matching local room (fallback to first active room, same as handleSyncReq); skip room 0 (JES-846).
  int ri = -1;
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (rooms[i].active && memcmp(rooms[i].id.pub_key, room_hash, 4) == 0) {
      ri = i; break;
    }
  }
  if (ri < 0) {
    for (int i = 1; i < MAX_ROOMS; i++) {
      if (rooms[i].active) { ri = i; break; }
    }
  }

  peers[pi].last_syncend_ts = getRTCClock()->getCurrentTime();
  Serial.printf("[SYNC] SYNCEND from peer[%d] room[%d]\n", pi, ri);

  if (ri < 0) return;  // no active room — nothing to push

  // --- Reverse push: send peer any posts it doesn't have yet ---
  // Parse peer's VV (embedded in SYNCEND payload).
  struct { uint8_t orig[4]; uint32_t seq; } peer_vv[MAX_VV_ORIGINS];
  for (int i = 0; i < num_vv; i++) {
    memcpy(peer_vv[i].orig, &data[10 + i * 8],     4);
    memcpy(&peer_vv[i].seq,  &data[10 + i * 8 + 4], 4);
  }
  auto peerKnows = [&](const uint8_t* orig) -> uint32_t {
    for (int i = 0; i < num_vv; i++) {
      if (memcmp(peer_vv[i].orig, orig, 4) == 0) return peer_vv[i].seq;
    }
    return 0;
  };

  calcPeerSecret(pi);
  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  int pushed = 0;
  uint32_t delay_ms = 300;  // small initial delay — peer's in-flight frames land first
  for (int k = 0; k < MAX_TOTAL_POSTS && pushed < MAX_SYNC_POSTS; k++) {
    const PostInfo& p = _post_pool[k];
    if (p.room_idx != (uint8_t)ri) continue;
    uint32_t peer_knows = peerKnows(p.origin_id);
    if (p.post_timestamp <= peer_knows) continue;
    if (isTombstoned(p.origin_id, p.post_timestamp)) continue;

    int dlen = 0;
    uint8_t buf[MAX_PACKET_PAYLOAD];
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    memcpy(&buf[dlen], &now, 4);               dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCDAT << 2);
    memcpy(&buf[dlen], room_hash, 4);          dlen += 4;
    memcpy(&buf[dlen], &p.post_timestamp, 4);  dlen += 4;
    memcpy(&buf[dlen], p.origin_id, 4);        dlen += 4;
    memcpy(&buf[dlen], p.author.pub_key, 4);   dlen += 4;
    const char* pname = resolveName(p.author.pub_key);
    uint8_t pname_len = (uint8_t)strlen(pname);
    if (pname_len > 23) pname_len = 23;
    buf[dlen++] = pname_len;
    memcpy(&buf[dlen], pname, pname_len);      dlen += pname_len;
    int max_tlen = (int)sizeof(buf) - dlen;
    if (max_tlen < 0) max_tlen = 0;
    int tlen = strlen(p.text);
    if (tlen > max_tlen) tlen = max_tlen;
    memcpy(&buf[dlen], p.text, tlen);          dlen += tlen;

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) {
      sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
      delay_ms += 500;
      pushed++;
    }
  }
  Serial.printf("[SYNC] reverse push → peer[%d]: %d post(s) for room[%d]\n",
                pi, pushed, ri);
  // Periodic timer (next_sync_at) is intentionally left at its normal 45 s cadence.
  // No SYNCEND is sent back: avoids infinite ping-pong; next SYNCREQ carries the VV.

  // JES-856: schedule a ROOMSYNC 2 s after sync completes to propagate room configs.
  // Multiple SYNCENDs (one per room) reset the same timer → one batched send.
  peers[pi].next_roomsync_at = futureMillis(2000UL);
}

/* ------------------------------------------------------------------ */
/*  Tombstone log (JES-824)                                             */
/* ------------------------------------------------------------------ */
#define TOMBSTONE_LOG_PATH    "/tombstone_log"
#define TOMBSTONE_LOG_TMP     "/tombstone_log.tmp"
#define TOMBSTONE_LOG_MAGIC_0 0x54   // 'T'
#define TOMBSTONE_LOG_MAGIC_1 0x42   // 'B'
#define TOMBSTONE_LOG_VERSION 1

void MultiRoomMesh::saveTombstones() {
  if (!_fs) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(TOMBSTONE_LOG_TMP, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(TOMBSTONE_LOG_TMP);
  File f = _fs->open(TOMBSTONE_LOG_TMP, FILE_O_WRITE);
#else
  File f = _fs->open(TOMBSTONE_LOG_TMP, "w", true);
#endif
  if (!f) return;
  uint8_t hdr[3] = { TOMBSTONE_LOG_MAGIC_0, TOMBSTONE_LOG_MAGIC_1, TOMBSTONE_LOG_VERSION };
  f.write(hdr, 3);
  f.write(&_tombstone_count, 1);
  for (uint8_t i = 0; i < _tombstone_count; i++) {
    f.write(_tombstones[i].origin_id, 4);
    f.write((const uint8_t*)&_tombstones[i].post_ts, 4);
    f.write(_tombstones[i].room_hash, 4);
  }
  f.close();
  _fs->remove(TOMBSTONE_LOG_PATH);
  _fs->rename(TOMBSTONE_LOG_TMP, TOMBSTONE_LOG_PATH);
}

void MultiRoomMesh::loadTombstones() {
  if (!_fs) return;
  if (_fs->exists(TOMBSTONE_LOG_TMP)) _fs->remove(TOMBSTONE_LOG_TMP);
#if defined(RP2040_PLATFORM)
  if (!_fs->exists(TOMBSTONE_LOG_PATH)) return;
  File f = _fs->open(TOMBSTONE_LOG_PATH, "r");
#else
  if (!_fs->exists(TOMBSTONE_LOG_PATH)) return;
  File f = _fs->open(TOMBSTONE_LOG_PATH);
#endif
  if (!f) return;
  uint8_t hdr[4];
  if (f.read(hdr, 4) != 4) { f.close(); return; }
  if (hdr[0] != TOMBSTONE_LOG_MAGIC_0 || hdr[1] != TOMBSTONE_LOG_MAGIC_1) { f.close(); return; }
  if (hdr[2] != TOMBSTONE_LOG_VERSION) { f.close(); return; }  // unknown version — start empty
  uint8_t cnt = hdr[3];
  if (cnt > MAX_TOMBSTONES) cnt = MAX_TOMBSTONES;
  _tombstone_count = 0;
  for (uint8_t i = 0; i < cnt; i++) {
    uint8_t  oid[4], rhash[4];
    uint32_t pts;
    if (f.read(oid, 4) != 4) break;
    if (f.read((uint8_t*)&pts, 4) != 4) break;
    if (f.read(rhash, 4) != 4) break;
    memcpy(_tombstones[_tombstone_count].origin_id, oid, 4);
    _tombstones[_tombstone_count].post_ts = pts;
    memcpy(_tombstones[_tombstone_count].room_hash, rhash, 4);
    _tombstone_count++;
  }
  f.close();
}

bool MultiRoomMesh::isTombstoned(const uint8_t* origin_id, uint32_t post_ts) {
  for (uint8_t i = 0; i < _tombstone_count; i++) {
    if (_tombstones[i].post_ts == post_ts &&
        memcmp(_tombstones[i].origin_id, origin_id, 4) == 0) return true;
  }
  return false;
}

void MultiRoomMesh::addTombstone(const uint8_t* origin_id, uint32_t post_ts,
                                  const uint8_t* room_hash) {
  // Idempotent: skip if already recorded
  if (isTombstoned(origin_id, post_ts)) return;
  // Evict oldest if full
  if (_tombstone_count == MAX_TOMBSTONES) {
    memmove(&_tombstones[0], &_tombstones[1], (MAX_TOMBSTONES - 1) * sizeof(Tombstone));
    _tombstone_count--;
  }
  memcpy(_tombstones[_tombstone_count].origin_id, origin_id, 4);
  _tombstones[_tombstone_count].post_ts = post_ts;
  memcpy(_tombstones[_tombstone_count].room_hash, room_hash, 4);
  _tombstone_count++;
}

bool MultiRoomMesh::deletePostEntry(uint8_t room_idx, const uint8_t* origin_id, uint32_t post_ts) {
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    if (_post_pool[i].room_idx == room_idx &&
        _post_pool[i].post_timestamp == post_ts &&
        memcmp(_post_pool[i].origin_id, origin_id, 4) == 0) {
      memset(&_post_pool[i], 0, sizeof(PostInfo));
      _post_pool[i].room_idx = 0xFF;
      if (room_idx < MAX_ROOMS && rooms[room_idx].num_posted > 0)
        rooms[room_idx].num_posted--;
      return true;
    }
  }
  return false;
}

bool MultiRoomMesh::handleDeletePost(uint8_t room_idx, const uint8_t* origin_id, uint32_t post_ts) {
  if (room_idx >= MAX_ROOMS || !rooms[room_idx].active) return false;
  const uint8_t* room_hash = rooms[room_idx].id.pub_key;
  // Record tombstone BEFORE clearing the post entry (prevents resurrection race)
  addTombstone(origin_id, post_ts, room_hash);
  bool found = deletePostEntry(room_idx, origin_id, post_ts);
  saveTombstones();
  if (found) {
    savePostPool();
    _post_dirty_at = 0;   // no pending dirty save needed
  }
  Serial.printf("[DEL] room[%d] origin=%02x%02x%02x%02x ts=%lu found=%d\n",
                (int)room_idx, origin_id[0], origin_id[1], origin_id[2], origin_id[3],
                (unsigned long)post_ts, found ? 1 : 0);
  emitSyncDel(room_hash, origin_id, post_ts);
  return found;
}

void MultiRoomMesh::emitSyncDel(const uint8_t* room_hash, const uint8_t* origin_id,
                                 uint32_t post_ts) {
  for (int pi = 0; pi < MAX_PEERS; pi++) {
    if (!peers[pi].active) continue;
    calcPeerSecret(pi);
    uint8_t buf[MAX_PACKET_PAYLOAD];
    int dlen = 0;
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    memcpy(&buf[dlen], &now, 4);              dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCDEL << 2);
    memcpy(&buf[dlen], room_hash, 4);         dlen += 4;
    memcpy(&buf[dlen], origin_id, 4);         dlen += 4;
    memcpy(&buf[dlen], &post_ts, 4);          dlen += 4;
    mesh::Identity peer_id;
    memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
    memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);
    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) sendFlood(pkt, (uint32_t)0, (uint8_t)(_prefs.path_hash_mode + 1));
  }
}

/**
 * Handle incoming SYNCDEL from peer pi (JES-824).
 * Wire format: [4:ts][1:flags][4:room_hash][4:origin_id][4:post_ts]
 * Total: 17 bytes minimum.
 */
void MultiRoomMesh::handleSyncDel(int pi, uint8_t* data, size_t len) {
  if (len < 17) return;  // [4:ts][1:flags][4:room_hash][4:origin_id][4:post_ts]
  uint8_t  room_hash[4];
  uint8_t  origin_id[4];
  uint32_t post_ts;
  memcpy(room_hash,  &data[5],  4);
  memcpy(origin_id,  &data[9],  4);
  memcpy(&post_ts,   &data[13], 4);
  DLOG(post_ts,
       "SYNC DEL←peer[%d] orig=%02x%02x%02x%02x ts=%lu",
       pi,
       (unsigned)origin_id[0], (unsigned)origin_id[1],
       (unsigned)origin_id[2], (unsigned)origin_id[3],
       (unsigned long)post_ts);

  // Find matching local room
  int ri = -1;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].active && memcmp(rooms[i].id.pub_key, room_hash, 4) == 0) {
      ri = i; break;
    }
  }
  if (ri < 0) return;  // room not known locally — ignore

  addTombstone(origin_id, post_ts, room_hash);
  bool found = deletePostEntry((uint8_t)ri, origin_id, post_ts);
  saveTombstones();
  if (found) savePostPool();
  Serial.printf("[SYNCDEL] peer[%d]: del ts=%lu room[%d] found=%d\n",
                pi, (unsigned long)post_ts, ri, found ? 1 : 0);
  // No re-broadcast — single-hop fanout is sufficient for small mesh
}

/* ------------------------------------------------------------------ */
/*  peer * CLI sub-commands                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::handlePeerCommand(char* args, char* reply, bool serial) {
  while (*args == ' ') args++;

  // "peer list" — list configured peer room servers
  if (strcmp(args, "list") == 0 || strcmp(args, "ls") == 0 || args[0] == 0) {
    if (serial) {
      Serial.printf("Peers (%d/%d configured):\n", _num_peers, MAX_PEERS);
      for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        Serial.printf("  [%d] '%s'  key=", i, peers[i].name);
        mesh::Utils::printHex(Serial, peers[i].pub_key, 6);
        Serial.printf("...  last_contact=%lu\n", (unsigned long)peers[i].last_contact);
      }
      reply[0] = 0;
    } else {
      // compact mesh-DM reply: "2/8: [0]Alice [1]Bob"
      int pos = sprintf(reply, "%d/%d:", _num_peers, MAX_PEERS);
      for (int i = 0; i < MAX_PEERS && pos < 140; i++) {
        if (!peers[i].active) continue;
        pos += snprintf(reply + pos, 160 - pos, " [%d]%s", i, peers[i].name);
      }
    }
    return;
  }

  // "peer add <hex64> <name>" — serial-only (structural config change)
  if (memcmp(args, "add ", 4) == 0) {
    if (!serial) { strcpy(reply, "Err - peer add only allowed via serial CLI"); return; }
    char* p = args + 4;
    while (*p == ' ') p++;
    // expect 64 hex chars = 32 bytes
    int hex_chars = 0;
    while (p[hex_chars] && p[hex_chars] != ' ') hex_chars++;
    if (hex_chars < 8) { strcpy(reply, "Err - need at least 4-byte hex pubkey"); return; }
    int byte_len = hex_chars / 2;
    if (byte_len > PUB_KEY_SIZE) byte_len = PUB_KEY_SIZE;
    uint8_t key[PUB_KEY_SIZE] = {};
    char saved = p[hex_chars];
    p[hex_chars] = '\0';
    bool hex_ok = mesh::Utils::fromHex(key, byte_len, p);
    p[hex_chars] = saved;
    if (!hex_ok) {
      strcpy(reply, "Err - bad hex pubkey");
      return;
    }
    char* name_p = p + hex_chars;
    while (*name_p == ' ') name_p++;
    // Find free slot
    for (int i = 0; i < MAX_PEERS; i++) {
      if (!peers[i].active) {
        peers[i].active = true;
        memcpy(peers[i].pub_key, key, PUB_KEY_SIZE);
        StrHelper::strncpy(peers[i].name, name_p[0] ? name_p : "peer", sizeof(peers[i].name));
        peers[i].last_contact = 0;
        peers[i].secret_valid = false;
        peers[i].next_sync_at = futureMillis(PEER_SYNC_BOOT_DELAY_MS);
        _num_peers++;
        savePeerConfig();
        sprintf(reply, "OK - peer[%d] '%s' added", i, peers[i].name);
        return;
      }
    }
    strcpy(reply, "Err - peer list full");
    return;
  }

  // "peer del <idx>" — serial-only (structural config change)
  if (memcmp(args, "del ", 4) == 0) {
    if (!serial) { strcpy(reply, "Err - peer del only allowed via serial CLI"); return; }
    int idx = atoi(args + 4);
    if (idx < 0 || idx >= MAX_PEERS || !peers[idx].active) {
      strcpy(reply, "Err - peer not active or invalid idx");
      return;
    }
    peers[idx].active = false;
    _num_peers--;
    savePeerConfig();
    strcpy(reply, "OK");
    return;
  }

  // "peer status" — show Phase 5 replication liveness
  if (strcmp(args, "status") == 0) {
    if (serial) {
      Serial.printf("Peers (%d/%d) — Phase 5 anti-entropy active:\n", _num_peers, MAX_PEERS);
      for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        Serial.printf("  [%d] '%s'  key=", i, peers[i].name);
        mesh::Utils::printHex(Serial, peers[i].pub_key, 4);
        Serial.printf("...  last=%lu  next_sync=%lums\n",
                      (unsigned long)peers[i].last_contact,
                      (unsigned long)peers[i].next_sync_at);
      }
      reply[0] = 0;
    } else {
      int pos = sprintf(reply, "%d peers(sync):", _num_peers);
      for (int i = 0; i < MAX_PEERS && pos < 130; i++) {
        if (!peers[i].active) continue;
        pos += snprintf(reply + pos, 160 - pos, " [%d]%s@%lu",
                        i, peers[i].name, (unsigned long)peers[i].last_contact);
      }
    }
    return;
  }

  // "peer sync [<idx>]" — trigger immediate SYNCREQ to all peers or one peer
  if (memcmp(args, "sync", 4) == 0 && (args[4] == ' ' || args[4] == 0)) {
    const char* p = args + 4;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
      int idx = atoi(p);
      if (idx < 0 || idx >= MAX_PEERS || !peers[idx].active) {
        strcpy(reply, "Err - peer not active or invalid idx");
        return;
      }
      sendSyncReq(idx);
      sprintf(reply, "OK - SYNCREQ sent to peer[%d] '%s'", idx, peers[idx].name);
    } else {
      int sent = 0;
      for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        sendSyncReq(i);
        sent++;
      }
      sprintf(reply, "OK - SYNCREQ sent to %d peer(s)", sent);
    }
    return;
  }

  // "peer fullsync [<idx>]" — full resync (empty VV) to recover pre-upgrade posts (JES-874)
  if (memcmp(args, "fullsync", 8) == 0 && (args[8] == ' ' || args[8] == 0)) {
    const char* p = args + 8;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
      int idx = atoi(p);
      if (idx < 0 || idx >= MAX_PEERS || !peers[idx].active) {
        strcpy(reply, "Err - peer not active or invalid idx");
        return;
      }
      sendSyncReq(idx, true);
      sprintf(reply, "OK - full SYNCREQ sent to peer[%d] '%s'", idx, peers[idx].name);
    } else {
      int sent = 0;
      for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active) continue;
        sendSyncReq(i, true);
        sent++;
      }
      sprintf(reply, "OK - full SYNCREQ sent to %d peer(s)", sent);
    }
    return;
  }

  strcpy(reply, "Err - usage: peer list|add <hex> <name>|del <idx>|status|sync [<idx>]|fullsync [<idx>]");
}

/* ------------------------------------------------------------------ */
/*  Notify target CLI (JES-834)                                         */
/* ------------------------------------------------------------------ */
// Handled in handleCommand() at the top-level dispatch.
// "notify <room_idx> add <hex64>"   — add notification target (web+serial)
// "notify <room_idx> del <hex64>"   — remove notification target (web+serial)
// "notify <room_idx> list"          — list targets (web+serial)

/* ------------------------------------------------------------------ */
/*  Peer management — web UI API (JES-816)                              */
/* ------------------------------------------------------------------ */

/**
 * Add a peer from the web UI (admin-authenticated by caller).
 * pub_key must be PUB_KEY_SIZE (32) bytes.
 * Returns peer index on success, -1 on failure (full or duplicate).
 */

// ---------------------------------------------------------------------------
// ACL management (JES-720) — web UI set permissions
// ---------------------------------------------------------------------------
bool MultiRoomMesh::setRoomClientPerm(int room, const char* pub_hex8, uint8_t perms) {
  if (room < 0 || room >= MAX_ROOMS || !rooms[room].active) return false;
  if (!pub_hex8 || strlen(pub_hex8) < 2) return false;
  int hex_len = (int)strlen(pub_hex8);
  if (hex_len > PUB_KEY_SIZE * 2) hex_len = PUB_KEY_SIZE * 2;
  // hex_len must be even
  if (hex_len % 2) hex_len--;
  uint8_t key_buf[PUB_KEY_SIZE] = {};
  for (int i = 0; i < hex_len / 2; i++) {
    char hi = pub_hex8[i * 2], lo = pub_hex8[i * 2 + 1];
    auto hexdig = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    key_buf[i] = (hexdig(hi) << 4) | hexdig(lo);
  }
  return rooms[room].acl.applyPermissions(rooms[room].id, key_buf, hex_len / 2, perms);
}

int MultiRoomMesh::addPeerFromWeb(const uint8_t* pub_key, const char* name) {
  // Duplicate check
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && memcmp(peers[i].pub_key, pub_key, PUB_KEY_SIZE) == 0)
      return -1;
  }
  // Find free slot
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].active) {
      peers[i].active      = true;
      memcpy(peers[i].pub_key, pub_key, PUB_KEY_SIZE);
      StrHelper::strncpy(peers[i].name,
                         (name && name[0]) ? name : "peer",
                         sizeof(peers[i].name));
      peers[i].last_contact = 0;
      peers[i].secret_valid = false;
      peers[i].next_sync_at     = futureMillis(PEER_SYNC_BOOT_DELAY_MS);
      peers[i].next_roomsync_at = futureMillis(PEER_ROOMSYNC_INTERVAL_MS);  // immediate push below; next in 10 min
      _num_peers++;
      savePeerConfig();
      triggerRoomSync(i);  // JES-848: push all rooms to new peer (deferred to loop, JES-864)
      return i;
    }
  }
  return -1;  // full
}

/**
 * Remove peer by index from the web UI.
 * Returns true on success.
 */
bool MultiRoomMesh::delPeerFromWeb(int idx) {
  if (idx < 0 || idx >= MAX_PEERS || !peers[idx].active) return false;
  peers[idx].active       = false;
  peers[idx].secret_valid = false;
  _num_peers--;
  savePeerConfig();
  return true;
}

/**
 * Trigger immediate SYNCREQ to one peer (idx >= 0) or all peers (idx == -1).
 */
void MultiRoomMesh::triggerPeerSync(int idx) {
  // Runs on the AsyncTCP web task — do NOT TX here (JES-864). Record the request
  // and let loop() (mesh task) send it. Coalesce differing targets to "all".
  if (_web_syncreq_pending && _web_syncreq_idx != idx) idx = -1;
  _web_syncreq_idx = idx;
  _web_syncreq_pending = true;
}

/**
 * JES-874: request a FULL anti-entropy resync (empty VV) to one peer (idx >= 0)
 * or all peers (idx == -1). Deferred to loop() like triggerPeerSync — never TX
 * from the AsyncTCP web task. Used to recover pre-upgrade posts that normal
 * incremental sync can no longer pull because of a stuck per-origin watermark.
 */
void MultiRoomMesh::triggerFullSync(int idx) {
  if (_web_fullsync_pending && _web_fullsync_idx != idx) idx = -1;
  _web_fullsync_idx = idx;
  _web_fullsync_pending = true;
}

/* ------------------------------------------------------------------ */
/*  Room-key propagation — JES-848                                      */
/* ------------------------------------------------------------------ */

/**
 * Send all active rooms (slots 1+) to peer pi via ECDH-encrypted DM.
 *
 * Wire format per room (JES-856: extended with guest_password):
 *   [4:ts][1:flags=TXT_TYPE_ROOMSYNC<<2][1:room_idx][64:prv_key][32:pub_key][name\0][guest_password\0]
 * Total: 4+1+1+96+max24+max16 = max 142 bytes — well within MAX_PACKET_PAYLOAD.
 *
 * Room 0 (node identity) is NEVER sent.
 * SECURITY: private key bytes are in the encrypted payload only — never logged.
 * Passwords are also in the encrypted ECDH payload only — never logged.
 */
void MultiRoomMesh::sendRoomSync(int pi) {
  if (!peers[pi].active) return;
  calcPeerSecret(pi);

  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  uint32_t stagger_ms = 0;
  int rooms_sent = 0;

  for (int ri = 1; ri < MAX_ROOMS; ri++) {  // room 0 always skipped
    if (!rooms[ri].active) continue;

    int len = 0;
    // Use modification timestamp (config_ts) so remote end can apply last-writer-wins (JES-860).
    uint32_t cts = rooms[ri].config_ts;
    memcpy(&reply_data[len], &cts, 4);             len += 4;
    reply_data[len++] = (TXT_TYPE_ROOMSYNC << 2);
    reply_data[len++] = (uint8_t)ri;               // room_idx (informational)

    // Write identity: prv_key[64] || pub_key[32] = 96 bytes
    size_t id_written = rooms[ri].id.writeTo(reply_data + len, sizeof(reply_data) - len);
    if (id_written != (PRV_KEY_SIZE + PUB_KEY_SIZE)) continue;  // buffer overflow guard
    len += (int)id_written;

    // Name: null-terminated, max 23 chars
    uint8_t name_len = (uint8_t)strnlen(rooms[ri].name, 23);
    memcpy(&reply_data[len], rooms[ri].name, name_len);  len += name_len;
    reply_data[len++] = 0;  // null terminator
    // guest_password: null-terminated, max 15 chars (JES-856)
    uint8_t gp_len = (uint8_t)strnlen(rooms[ri].guest_password, sizeof(rooms[ri].guest_password) - 1);
    memcpy(&reply_data[len], rooms[ri].guest_password, gp_len);  len += gp_len;
    reply_data[len++] = 0;  // null terminator

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, reply_data, len);
    if (pkt) {
      sendFlood(pkt, stagger_ms, (uint8_t)(_prefs.path_hash_mode + 1));
      stagger_ms += 1000;
      rooms_sent++;
    }
  }

  // SECURITY: log only peer name + count — private keys never appear in logs
  Serial.printf("[ROOMSYNC] sent %d room(s) → peer[%d] '%s'\n",
                rooms_sent, pi, peers[pi].name);
}

/**
 * Handle incoming ROOMSYNC frame from peer pi (JES-848, extended JES-856).
 *
 * Wire format: [4:ts][1:flags][1:room_idx][64:prv_key][32:pub_key][name\0][guest_password\0]
 * Minimum valid length: 4+1+1+64+32+1 = 103 bytes.
 *
 * Room 0 is NEVER accepted. Identified by pub_key.
 * - New room (pub_key not found): install key + name + guest_password in a free slot.
 * - Existing room (pub_key found): update name + guest_password (last-writer-wins, JES-856).
 * SECURITY: private key and passwords are never logged.
 */
void MultiRoomMesh::handleRoomSync(int pi, uint8_t* data, size_t len) {
  // Minimum: ts[4] + flags[1] + room_idx[1] + prv[64] + pub[32] + NUL[1] = 103
  DLOG((uint32_t)(millis()/1000),
       "SYNC ROOM←peer[%d] len=%d pub=%02x%02x%02x%02x",
       pi, (int)len,
       len >= 103 ? (unsigned)data[70] : 0u,
       len >= 103 ? (unsigned)data[71] : 0u,
       len >= 103 ? (unsigned)data[72] : 0u,
       len >= 103 ? (unsigned)data[73] : 0u);
  if (len < 103) {
    Serial.printf("[ROOMSYNC] ignored — frame too short (%u < 103)\n", (unsigned)len);
    return;
  }

  // Parse public key at offset 70 (4+1+1+64)
  const uint8_t* recv_pub = &data[70];

  // Room 0 guard: never accept room 0 identity
  if (memcmp(recv_pub, rooms[0].id.pub_key, PUB_KEY_SIZE) == 0) {
    Serial.printf("[ROOMSYNC] rejected — pub matches room0 identity\n");
    return;
  }

  // Helper: parse NUL-terminated string from payload at given offset, max max_len chars.
  // Returns length (excluding NUL), advances offset past the NUL.
  auto parseNulStr = [&](size_t offset, char* dst, size_t dst_max) -> size_t {
    if (offset >= len) { if (dst) dst[0] = 0; return 0; }
    size_t avail = len - offset;
    size_t sl = strnlen((char*)&data[offset], avail < dst_max ? avail : dst_max);
    if (dst) {
      memcpy(dst, &data[offset], sl);
      dst[sl] = 0;
    }
    return sl;
  };

  // Check if we already have a room with this pub_key (JES-856: update, not skip).
  for (int i = 1; i < MAX_ROOMS; i++) {  // never match slot 0
    if (!rooms[i].active) continue;
    if (memcmp(rooms[i].id.pub_key, recv_pub, PUB_KEY_SIZE) != 0) continue;

    // Existing room found — update name and guest_password (last-writer-wins, JES-860).
    // F1: reject stale frames; accept only if recv_ts is strictly newer than stored config_ts.
    uint32_t recv_ts;
    memcpy(&recv_ts, data, 4);
    if (recv_ts <= rooms[i].config_ts) {
      Serial.printf("[ROOMSYNC] skipped update room[%d] '%s' — recv_ts %u <= stored %u (stale)\n",
                    i, rooms[i].name, (unsigned)recv_ts, (unsigned)rooms[i].config_ts);
      return;
    }

    // Parse new values into local buffers BEFORE taking mutex (pure read of data[]).
    char new_name[24] = {};
    char new_gp[16]   = {};
    size_t nl         = 0;
    size_t gp_offset  = 0;
    if (len > 102) {
      nl        = parseNulStr(102, new_name, 23);
      gp_offset = 102 + nl + 1;
      if (gp_offset < len) parseNulStr(gp_offset, new_gp, 15);
    }

    // Lock rooms[] for the write — protect against AsyncTCP web-handler reads (JES-865).
    if (!lockRooms(100)) {
      Serial.printf("[ROOMSYNC] skipped update room[%d] — mutex timeout\n", i);
      return;
    }
    bool changed = false;
    if (nl > 0 && strncmp(rooms[i].name, new_name, sizeof(rooms[i].name)) != 0) {
      StrHelper::strncpy(rooms[i].name, new_name, sizeof(rooms[i].name));
      changed = true;
    }
    // F2: only update guest_password when the gp field was present in the frame.
    if (gp_offset < len && strncmp(rooms[i].guest_password, new_gp, sizeof(rooms[i].guest_password)) != 0) {
      StrHelper::strncpy(rooms[i].guest_password, new_gp, sizeof(rooms[i].guest_password));
      changed = true;
    }
    if (changed) rooms[i].config_ts = recv_ts;  // F1: advance stored ts
    char log_name[24];  // copy for logging after mutex release
    StrHelper::strncpy(log_name, rooms[i].name, sizeof(log_name));
    unlockRooms();  // release BEFORE slow SPIFFS write

    if (changed) saveRoomConfig();
    // SECURITY: log only slot + name — passwords never logged
    Serial.printf("[ROOMSYNC] updated room[%d] '%s' from peer[%d] '%s'\n",
                  i, log_name, pi, peers[pi].name);
    return;
  }

  // Extract recv_ts for use in install branch (F1: seed config_ts on first install).
  uint32_t recv_ts;
  memcpy(&recv_ts, data, 4);

  // Validate private key (basic format check: reject all-0x00 or all-0xFF)
  const uint8_t* recv_prv = &data[6];
  if (!mesh::LocalIdentity::validatePrivateKey(recv_prv)) {
    Serial.printf("[ROOMSYNC] rejected — invalid private key format\n");
    return;
  }

  // Find a free slot (slot 0 is node identity, never overwrite)
  int free_slot = -1;
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) { free_slot = i; break; }
  }
  if (free_slot < 0) {
    Serial.printf("[ROOMSYNC] ignored — no free room slots (MAX_ROOMS=%d full)\n", MAX_ROOMS);
    return;
  }

  // Parse name and guest_password from frame into local buffers (no mutex needed yet).
  char inst_name[24] = {};
  char inst_gp[16]   = {};
  size_t inst_name_len = 0;
  if (len > 102) {
    inst_name_len = parseNulStr(102, inst_name, 23);
    size_t gp_off = 102 + inst_name_len + 1;
    if (gp_off < len) parseNulStr(gp_off, inst_gp, 15);
  }
  if (inst_name[0] == 0) {
    snprintf(inst_name, sizeof(inst_name), "Room%d", free_slot);
  }

  // Lock rooms[] for the install write — protect against AsyncTCP reads (JES-865).
  if (!lockRooms(100)) {
    Serial.printf("[ROOMSYNC] ignored install — mutex timeout\n");
    return;
  }
  // Install identity (prv[64] || pub[32] = 96 bytes at offset 6).
  rooms[free_slot].id.readFrom(&data[6], PRV_KEY_SIZE + PUB_KEY_SIZE);
  StrHelper::strncpy(rooms[free_slot].name, inst_name, sizeof(rooms[free_slot].name));
  StrHelper::strncpy(rooms[free_slot].guest_password, inst_gp, sizeof(rooms[free_slot].guest_password));
  rooms[free_slot].config_ts = recv_ts;  // seed LWW timestamp (JES-860, F1)
  rooms[free_slot].active    = true;
  rooms[free_slot].stealth   = true;     // stealth by default (JES-772)
  StrHelper::strncpy(rooms[free_slot].password, _prefs.password,
                     sizeof(rooms[free_slot].password));
  _num_active_rooms++;
  unlockRooms();  // release BEFORE slow SPIFFS writes

  saveRoomIdentity(free_slot);
  saveRoomConfig();

  // SECURITY: log only 4-byte pub prefix + name — private key and passwords never logged
  Serial.printf("[ROOMSYNC] room '%s' installed in slot %d (pub: %02X%02X%02X%02X) from peer[%d] '%s'\n",
                rooms[free_slot].name, free_slot,
                recv_pub[0], recv_pub[1], recv_pub[2], recv_pub[3],
                pi, peers[pi].name);
}

/**
 * Push all active rooms (1+) to one peer (idx >= 0) or all peers (idx == -1).
 */
void MultiRoomMesh::triggerRoomSync(int idx) {
  // May run on the AsyncTCP web task (room create/rename/rekey/manual push) —
  // do NOT TX here (JES-864). Record the request; loop() (mesh task) sends it.
  // Coalesce differing targets to "all". Safe when called from the mesh task too.
  if (_web_roomsync_pending && _web_roomsync_idx != idx) idx = -1;
  _web_roomsync_idx = idx;
  _web_roomsync_pending = true;
}

/* ------------------------------------------------------------------ */
/*  Flood helpers                                                       */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::sendFloodScoped(const TransportKey& scope,
                                     mesh::Packet* pkt,
                                     uint32_t delay_millis,
                                     uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MultiRoomMesh::sendFloodReply(mesh::Packet* packet,
                                    unsigned long delay_millis,
                                    uint8_t path_hash_size) {
  if (recv_pkt_region && !recv_pkt_region->isWildcard()) {
    TransportKey scope;
    if (region_map.getTransportKeysFor(*recv_pkt_region, &scope, 1) > 0) {
      sendFloodScoped(scope, packet, delay_millis, path_hash_size);
    } else {
      sendFlood(packet, delay_millis, path_hash_size);
    }
  } else {
    sendFlood(packet, delay_millis, path_hash_size);
  }
}

/* ------------------------------------------------------------------ */
/*  Packet routing overrides                                            */
/* ------------------------------------------------------------------ */
bool MultiRoomMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    recv_pkt_region = (region_map.getWildcard().flags & REGION_DENY_FLOOD) ?
                       nullptr : &region_map.getWildcard();
  } else {
    recv_pkt_region = nullptr;
  }
  return false;
}

bool MultiRoomMesh::allowPacketForward(const mesh::Packet* packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()) {
    if (packet->getPathHashCount() >= _prefs.flood_max) return false;
    if (packet->getRouteType() == ROUTE_TYPE_FLOOD &&
        packet->getPathHashCount() >= _prefs.flood_max_unscoped) return false;
    if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT &&
        packet->getPathHashCount() >= _prefs.flood_max_advert) return false;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/*  Timing helpers                                                      */
/* ------------------------------------------------------------------ */
int MultiRoomMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

const char* MultiRoomMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
  return tmp;
}

uint32_t MultiRoomMesh::getRetransmitDelay(const mesh::Packet* packet) {
  uint32_t t = _radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2)
               * _prefs.tx_delay_factor;
  return getRNG()->nextInt(0, 5 * t + 1);
}

uint32_t MultiRoomMesh::getDirectRetransmitDelay(const mesh::Packet* packet) {
  uint32_t t = _radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2)
               * _prefs.direct_tx_delay_factor;
  return getRNG()->nextInt(0, 5 * t + 1);
}

/* ------------------------------------------------------------------ */
/*  CommonCLICallbacks implementations                                  */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr,
                                          int timeout_mins) {
  set_radio_at    = futureMillis(2000);
  pending_freq    = freq;
  pending_bw      = bw;
  pending_sf      = sf;
  pending_cr      = cr;
  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000);
}

bool MultiRoomMesh::formatFileSystem() {
#if defined(ESP32)
  return SPIFFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(NRF52_PLATFORM)
  return InternalFS.format();
#else
  return false;
#endif
}

void MultiRoomMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

void MultiRoomMesh::formatStatsReply(char* reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MultiRoomMesh::formatRadioStatsReply(char* reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver,
                                       getTotalAirTime(), getReceiveAirTime());
}

void MultiRoomMesh::formatPacketStatsReply(char* reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver,
                                        getNumSentFlood(), getNumSentDirect(),
                                        getNumRecvFlood(), getNumRecvDirect());
}

void MultiRoomMesh::saveIdentity(const mesh::LocalIdentity& new_id) {
  // Save as room[0] identity (used by CommonCLI 'set id' command)
  rooms[0].id = new_id;
  self_id = new_id;
  saveRoomIdentity(0);
}

void MultiRoomMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables*)getTables())->resetStats();
}

void MultiRoomMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MultiRoomMesh::saveRegions() {
  return region_map.save(_fs);
}

void MultiRoomMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}
