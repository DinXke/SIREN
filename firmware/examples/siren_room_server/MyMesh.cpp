#include "MyMesh.h"

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
      telemetry(MAX_PACKET_PAYLOAD - 4)
{
  _fs = nullptr;
  _active_slot = 0;
  _num_active_rooms = 0;
  last_millis = 0;
  uptime_millis = 0;
  _logging = false;
  region_load_active = false;
  set_radio_at = revert_radio_at = 0;
  _post_dirty_at = 0;
  _mqtt_post_cb  = nullptr;
  _mqtt_post_ctx = nullptr;

  memset(_names, 0, sizeof(_names));
  _name_lru_ctr = 0;

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
  _prefs.disable_fwd  = 1;
  _prefs.advert_interval       = 1;   // 2 min
  _prefs.flood_advert_interval = 47;  // 47 h
  _prefs.flood_max         = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert  = 8;
  _prefs.interference_threshold = 0;
  StrHelper::strncpy(_prefs.node_name, "SIREN", sizeof(_prefs.node_name));
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));

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
  _advert_interval_sec = 120;

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

  _cli.loadPrefs(_fs);

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
    StrHelper::strncpy(rooms[0].password, ADMIN_PASSWORD, sizeof(rooms[0].password));
    _num_active_rooms = 1;
    loadOrCreateRoomIdentity(0);
    saveRoomConfig();
  }

  loadPeerConfig();
  // Schedule staggered initial sync for each configured peer (Phase 5)
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active) {
      peers[i].next_sync_at = futureMillis(PEER_SYNC_BOOT_DELAY_MS + (uint32_t)i * 5000);
    }
  }
  loadPostPool();   // restore persisted messages (JES-787)
  loadNameTable();  // restore advertised-name cache (JES-798)

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
  if (ais < 10 || ais > 3600) ais = 120;
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
  if (sec > 3600) sec = 3600;
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
/*  onRecvPacket — multi-room routing override                          */
/* ------------------------------------------------------------------ */
mesh::DispatcherAction MultiRoomMesh::onRecvPacket(mesh::Packet* pkt) {
  uint8_t ptype = pkt->getPayloadType();

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
/*  onAnonDataRecv — handles login (ANON_REQ)                          */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                   const mesh::Identity& sender,
                                   uint8_t* data, size_t len) {
  if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;

  RoomSlot& slot = rooms[_active_slot];

  uint32_t sender_timestamp, sender_sync_since;
  memcpy(&sender_timestamp,   data,      4);
  memcpy(&sender_sync_since, &data[4],   4);
  data[len] = 0;

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
  }

  if (packet->isRouteFlood()) {
    client->out_path_len = OUT_PATH_UNKNOWN;
  }

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
      if      (flags == TXT_TYPE_SYNCREQ) handleSyncReq(pi, data, len);
      else if (flags == TXT_TYPE_SYNCDAT) handleSyncDat(pi, data, len);
      else if (flags == TXT_TYPE_SYNCEND) handleSyncEnd(pi, data, len);
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
          if (!is_retry) addPost(slot, client, (const char*)&data[5]);
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
void MultiRoomMesh::addPost(RoomSlot& slot, ClientInfo* client, const char* text) {
  uint8_t ridx = (uint8_t)(&slot - rooms);
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
  free_slot->room_idx       = ridx;
  // Phase 5: tag with this room-server as origin
  memcpy(free_slot->origin_id, slot.id.pub_key, 4);

  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  slot.num_posted++;
  _post_dirty_at = futureMillis(5000);  // debounced persist (JES-794)

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

void MultiRoomMesh::pushPostToClient(RoomSlot& slot, ClientInfo* client, PostInfo& post) {
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
  // Build advert data using the room's own name + location
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = 0;

  // Use the standard ADV_TYPE_ROOM format
  app_data[app_data_len++] = ADV_TYPE_ROOM;

  // Encode name (null-terminated, up to 20 chars)
  int name_len = strlen(slot.name);
  if (name_len > 20) name_len = 20;
  app_data[app_data_len++] = (uint8_t)name_len;
  memcpy(&app_data[app_data_len], slot.name, name_len);
  app_data_len += name_len;

  // Optionally encode lat/lon if non-zero
  if (slot.lat != 0.0f || slot.lon != 0.0f) {
    // encode as 4-byte floats
    if (app_data_len + 8 <= MAX_ADVERT_DATA_SIZE) {
      memcpy(&app_data[app_data_len], &slot.lat, 4); app_data_len += 4;
      memcpy(&app_data[app_data_len], &slot.lon, 4); app_data_len += 4;
    }
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
        slot.next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + (uint32_t)i * 15000);
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
      peers[pi].next_sync_at = futureMillis(PEER_SYNC_INTERVAL_MS);
    }
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
        memcmp(command, "region ", 7) == 0) {
      strcpy(reply, "Err - repeater settings only available on management room");
      return;
    }
  }

  // ---- peer management commands (Phase 5 ground work) ----
  if (memcmp(command, "peer", 4) == 0 && (command[4] == ' ' || command[4] == 0)) {
    handlePeerCommand(command + 4, reply, is_serial);
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
    if (ridx < 0 || ridx >= MAX_ROOMS || !rooms[ridx].active) {
      strcpy(reply, "Err: invalid room idx"); return;
    }
    if (*arg == 0) { strcpy(reply, "Err: missing text"); return; }
    addServerPost(ridx, arg);
    snprintf(reply, 60, "OK - posted to room %d", ridx);
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
      saveRoomConfig();
      strcpy(reply, "OK");
    } else if (memcmp(p, "pass ", 5) == 0) {
      StrHelper::strncpy(rooms[idx].password, p + 5, sizeof(rooms[idx].password));
      saveRoomConfig();
      strcpy(reply, "OK");
    } else if (memcmp(p, "guest ", 6) == 0) {
      StrHelper::strncpy(rooms[idx].guest_password, p + 6, sizeof(rooms[idx].guest_password));
      saveRoomConfig();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - unknown field (use name|pass|guest)");
    }
    return;
  }

  // "room add" — activate next free slot
  if (strcmp(args, "add") == 0) {
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!rooms[i].active) {
        rooms[i].active = true;
        snprintf(rooms[i].name, sizeof(rooms[i].name), "Room%d", i);
        StrHelper::strncpy(rooms[i].password, ADMIN_PASSWORD, sizeof(rooms[i].password));
        rooms[i].guest_password[0] = 0;
        loadOrCreateRoomIdentity(i);
        _num_active_rooms++;
        saveRoomConfig();
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

  strcpy(reply, "Err - usage: room list|add|del <idx>|set <idx> name|pass|guest <val>|stealth <idx> on|off|qr <idx>|clients <idx>|setperm <idx> <hex> <perms>|status <idx>");
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
#define POST_LOG_PATH "/post_log"
#define POST_LOG_MAGIC_0 0x50   // 'P'
#define POST_LOG_MAGIC_1 0x4F   // 'O'
#define POST_LOG_MAGIC_2 0x53   // 'S'
#define POST_LOG_MAGIC_3 0x54   // 'T'
#define POST_LOG_VERSION 2      // bumped: added origin_id[4] per slot

void MultiRoomMesh::savePostPool() {
  if (!_fs) return;
#if defined(RP2040_PLATFORM)
  File f = _fs->open(POST_LOG_PATH, "w");
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(POST_LOG_PATH);
  File f = _fs->open(POST_LOG_PATH, FILE_O_WRITE);
#else
  File f = _fs->open(POST_LOG_PATH, "w", true);
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
}

void MultiRoomMesh::loadPostPool() {
  if (!_fs) return;
#if defined(RP2040_PLATFORM)
  if (!_fs->exists(POST_LOG_PATH)) return;
  File f = _fs->open(POST_LOG_PATH, "r");
#else
  if (!_fs->exists(POST_LOG_PATH)) return;
  File f = _fs->open(POST_LOG_PATH);
#endif
  if (!f) return;

  // Validate header
  uint8_t hdr[6];
  if (f.read(hdr, 6) != 6) { f.close(); return; }
  if (hdr[0] != POST_LOG_MAGIC_0 || hdr[1] != POST_LOG_MAGIC_1 ||
      hdr[2] != POST_LOG_MAGIC_2 || hdr[3] != POST_LOG_MAGIC_3 ||
      hdr[4] != POST_LOG_VERSION || hdr[5] != (uint8_t)MAX_TOTAL_POSTS) {
    f.close();
    return;   // format mismatch (or old v1); start fresh — posts are transient
  }

  // Read all slots
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    PostInfo& p = _post_pool[i];
    if (f.read(p.author.pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE) break;
    if (f.read((uint8_t*)&p.post_timestamp, 4) != 4) break;
    if (f.read((uint8_t*)p.text, MAX_POST_TEXT_LEN + 1) != MAX_POST_TEXT_LEN + 1) break;
    if (f.read(&p.room_idx, 1) != 1) break;
    if (f.read(p.origin_id, 4) != 4) break;  // Phase 5

    // Prune posts for rooms that are no longer active
    if (p.room_idx != 0xFF &&
        (p.room_idx >= MAX_ROOMS || !rooms[p.room_idx].active)) {
      memset(&p, 0, sizeof(PostInfo));
      p.room_idx = 0xFF;
    }
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
}

/* ------------------------------------------------------------------ */
/*  Name resolution table (JES-798)                                     */
/* ------------------------------------------------------------------ */
#define NAMES_PATH "/names"
#define NAMES_MAGIC_0 0x4E   // 'N'
#define NAMES_MAGIC_1 0x4D   // 'M'
#define NAMES_VERSION    1

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
      hdr[2] != NAMES_VERSION || hdr[3] != (uint8_t)NAME_TABLE_SIZE) {
    f.close(); return;
  }
  uint32_t max_seq = 0;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (f.read(_names[i].pub_prefix, NAME_KEY_SIZE) != NAME_KEY_SIZE) break;
    if (f.read((uint8_t*)_names[i].name, sizeof(_names[i].name)) != sizeof(_names[i].name)) break;
    if (f.read((uint8_t*)&_names[i].lru_seq, 4) != 4) break;
    _names[i].name[sizeof(_names[i].name) - 1] = 0;  // ensure NUL
    if (_names[i].lru_seq > max_seq) max_seq = _names[i].lru_seq;
  }
  f.close();
  _name_lru_ctr = max_seq;
}

void MultiRoomMesh::onAdvertRecv(mesh::Packet* /*pkt*/, const mesh::Identity& id,
                                  uint32_t /*ts*/,
                                  const uint8_t* app_data, size_t app_data_len) {
  AdvertDataParser parser(app_data, (uint8_t)app_data_len);
  if (!parser.isValid() || !parser.hasName()) return;
  const char* adv_name = parser.getName();
  if (!adv_name || adv_name[0] == 0) return;

  // Find existing entry or lowest-seq victim
  int victim = 0;
  uint32_t min_seq = UINT32_MAX;
  for (int i = 0; i < NAME_TABLE_SIZE; i++) {
    if (_names[i].lru_seq == 0) {
      // Empty slot — use immediately
      victim = i;
      min_seq = 0;
      break;
    }
    if (memcmp(_names[i].pub_prefix, id.pub_key, NAME_KEY_SIZE) == 0) {
      // Update existing entry
      StrHelper::strncpy(_names[i].name, adv_name, sizeof(_names[i].name));
      _names[i].lru_seq = ++_name_lru_ctr;
      saveNameTable();
      return;
    }
    if (_names[i].lru_seq < min_seq) { min_seq = _names[i].lru_seq; victim = i; }
  }

  // Fill victim slot
  memcpy(_names[victim].pub_prefix, id.pub_key, NAME_KEY_SIZE);
  StrHelper::strncpy(_names[victim].name, adv_name, sizeof(_names[victim].name));
  _names[victim].lru_seq = ++_name_lru_ctr;
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

void MultiRoomMesh::addServerPost(int room_idx, const char* text) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || !rooms[room_idx].active) return;
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
    json += "{\"name\":\"";
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
                                    const char* text) {
  if (ridx >= MAX_ROOMS || !rooms[ridx].active) return false;

  // Dedup check: already have this (origin, ts) pair?
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    if (_post_pool[i].room_idx == ridx &&
        _post_pool[i].post_timestamp == ts &&
        memcmp(_post_pool[i].origin_id, origin_id, 4) == 0) {
      return false;   // duplicate
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
  free_slot->room_idx       = ridx;
  memcpy(free_slot->origin_id, origin_id, 4);

  slot.num_posted++;
  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _post_dirty_at = futureMillis(5000);

  // Update VV
  vvUpdate(slot, origin_id, ts);
  return true;
}

/**
 * Push a single post to peer pi via SYNCDAT DM.
 * Wire format:  [4:ts][1:flags][4:post_ts][4:origin_id][4:author_pub][text]
 */
void MultiRoomMesh::pushPostToPeer(int pi, RoomSlot& slot, PostInfo& post) {
  calcPeerSecret(pi);

  int len = 0;
  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(&reply_data[len], &now, 4);                        len += 4;
  reply_data[len++] = (TXT_TYPE_SYNCDAT << 2);
  memcpy(&reply_data[len], &post.post_timestamp, 4);        len += 4;
  memcpy(&reply_data[len], post.origin_id, 4);              len += 4;
  memcpy(&reply_data[len], post.author.pub_key, 4);         len += 4;
  int text_len = strlen(post.text);
  if (text_len > MAX_POST_TEXT_LEN) text_len = MAX_POST_TEXT_LEN;
  memcpy(&reply_data[len], post.text, text_len);            len += text_len;

  // Send to peer's room[0] identity
  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  self_id = rooms[0].id;
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                             peers[pi].shared_secret, reply_data, len);
  if (pkt) sendFlood(pkt, (uint32_t)0, (uint8_t)(_prefs.path_hash_mode + 1));
}

/**
 * Send SYNCREQ to peer pi for rooms[0].
 * Wire format:  [4:ts][1:flags][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 */
void MultiRoomMesh::sendSyncReq(int pi) {
  if (!peers[pi].active || !rooms[0].active) return;
  calcPeerSecret(pi);

  RoomSlot& slot = rooms[0];
  int len = 0;
  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(&reply_data[len], &now, 4);         len += 4;
  reply_data[len++] = (TXT_TYPE_SYNCREQ << 2);

  uint8_t num_vv = 0;
  int vv_count_pos = len;
  reply_data[len++] = 0;   // placeholder for num_vv

  for (int i = 0; i < MAX_VV_ORIGINS && len + 8 <= (int)sizeof(reply_data); i++) {
    if (slot.vv[i].seq == 0) continue;
    memcpy(&reply_data[len], slot.vv[i].origin_id, 4); len += 4;
    memcpy(&reply_data[len], &slot.vv[i].seq,      4); len += 4;
    num_vv++;
  }
  reply_data[vv_count_pos] = num_vv;

  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  self_id = rooms[0].id;
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                             peers[pi].shared_secret, reply_data, len);
  if (pkt) sendFlood(pkt, (uint32_t)0, (uint8_t)(_prefs.path_hash_mode + 1));

  Serial.printf("[SYNC] SYNCREQ → peer[%d] '%s' (vv=%d)\n", pi, peers[pi].name, (int)num_vv);
}

/**
 * Handle incoming SYNCREQ from peer pi.
 * Parse their VV, find posts we have that they're missing, send SYNCDAT for each.
 * Wire format in: [4:ts][1:flags][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 */
void MultiRoomMesh::handleSyncReq(int pi, uint8_t* data, size_t len) {
  if (len < 6) return;
  uint8_t num_vv = data[5];
  if ((size_t)(6 + (int)num_vv * 8) > len) return;

  // Parse peer's VV into a local lookup (origin_id[4] → highest_ts)
  struct { uint8_t orig[4]; uint32_t seq; } peer_vv[MAX_VV_ORIGINS];
  uint8_t peer_vv_count = (num_vv > MAX_VV_ORIGINS) ? MAX_VV_ORIGINS : num_vv;
  for (int i = 0; i < peer_vv_count; i++) {
    memcpy(peer_vv[i].orig, &data[6 + i * 8],    4);
    memcpy(&peer_vv[i].seq,  &data[6 + i * 8 + 4], 4);
  }

  // Helper: look up peer's knowledge for a given origin
  auto peerKnows = [&](const uint8_t* orig) -> uint32_t {
    for (int i = 0; i < peer_vv_count; i++) {
      if (memcmp(peer_vv[i].orig, orig, 4) == 0) return peer_vv[i].seq;
    }
    return 0;
  };

  // For each post in rooms[0] that the peer is missing, send SYNCDAT
  if (!rooms[0].active) return;
  calcPeerSecret(pi);

  mesh::Identity peer_id;
  memset(peer_id.pub_key, 0, PUB_KEY_SIZE);
  memcpy(peer_id.pub_key, peers[pi].pub_key, PUB_KEY_SIZE);

  int sent = 0;
  uint32_t delay_ms = 500;   // stagger SYNCDAT frames to avoid packet pool exhaustion
  for (int k = 0; k < MAX_TOTAL_POSTS && sent < MAX_SYNC_POSTS; k++) {
    const PostInfo& p = _post_pool[k];
    if (p.room_idx != 0) continue;   // Phase 5: sync rooms[0] only
    uint32_t peer_knows = peerKnows(p.origin_id);
    if (p.post_timestamp <= peer_knows) continue;  // peer already has this

    int dlen = 0;
    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    uint8_t buf[MAX_PACKET_PAYLOAD];
    memcpy(&buf[dlen], &now, 4);                     dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCDAT << 2);
    memcpy(&buf[dlen], &p.post_timestamp, 4);        dlen += 4;
    memcpy(&buf[dlen], p.origin_id, 4);              dlen += 4;
    memcpy(&buf[dlen], p.author.pub_key, 4);         dlen += 4;
    int tlen = strlen(p.text);
    if (tlen > MAX_POST_TEXT_LEN) tlen = MAX_POST_TEXT_LEN;
    memcpy(&buf[dlen], p.text, tlen);                dlen += tlen;

    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) {
      sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
      delay_ms += 500;
      sent++;
    }
  }

  // Send SYNCEND with our VV
  {
    int dlen = 0;
    uint8_t buf[MAX_PACKET_PAYLOAD];
    uint32_t now2 = getRTCClock()->getCurrentTimeUnique();
    memcpy(&buf[dlen], &now2, 4);   dlen += 4;
    buf[dlen++] = (TXT_TYPE_SYNCEND << 2);
    uint8_t nvv = 0;
    int nvv_pos = dlen++;   // placeholder
    for (int i = 0; i < MAX_VV_ORIGINS && dlen + 8 <= (int)sizeof(buf); i++) {
      if (rooms[0].vv[i].seq == 0) continue;
      memcpy(&buf[dlen], rooms[0].vv[i].origin_id, 4); dlen += 4;
      memcpy(&buf[dlen], &rooms[0].vv[i].seq,      4); dlen += 4;
      nvv++;
    }
    buf[nvv_pos] = nvv;
    self_id = rooms[0].id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, peer_id,
                               peers[pi].shared_secret, buf, dlen);
    if (pkt) sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
  }

  Serial.printf("[SYNC] SYNCREQ from peer[%d]: sent %d post(s)\n", pi, sent);
}

/**
 * Handle incoming SYNCDAT from peer pi — ingest one post.
 * Wire format: [4:ts][1:flags][4:post_ts][4:origin_id][4:author_pub][text]
 */
void MultiRoomMesh::handleSyncDat(int pi, uint8_t* data, size_t len) {
  if (len < 17) return;
  uint32_t post_ts;
  uint8_t  origin_id[4], author_pub[4];
  memcpy(&post_ts,   &data[5],  4);
  memcpy(origin_id,  &data[9],  4);
  memcpy(author_pub, &data[13], 4);
  data[len] = 0;   // NUL-terminate text
  const char* text = (const char*)&data[17];

  bool added = ingestSyncPost(0, origin_id, post_ts, author_pub, text);
  if (added) {
    Serial.printf("[SYNC] SYNCDAT from peer[%d]: +post ts=%lu\n",
                  pi, (unsigned long)post_ts);
  }
}

/**
 * Handle incoming SYNCEND from peer pi — update our VV knowledge.
 * Wire format: [4:ts][1:flags][1:num_vv][N*8:VVEntry{origin[4],seq[4]}]
 */
void MultiRoomMesh::handleSyncEnd(int pi, uint8_t* data, size_t len) {
  if (len < 6 || !rooms[0].active) return;
  uint8_t num_vv = data[5];
  if ((size_t)(6 + (int)num_vv * 8) > len) return;
  // We intentionally don't merge the peer's VV into ours here — our VV
  // grows naturally as posts arrive.  SYNCEND is informational only for
  // Phase 5 (no multi-hop propagation yet).
  (void)num_vv;
  Serial.printf("[SYNC] SYNCEND from peer[%d]\n", pi);
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
    if (!mesh::Utils::fromHex(key, byte_len, p)) {
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

  strcpy(reply, "Err - usage: peer list|add <hex> <name>|del <idx>|status|sync [<idx>]");
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
