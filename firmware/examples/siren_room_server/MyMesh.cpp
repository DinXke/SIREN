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

  for (int i = 0; i < MAX_ROOMS; i++) {
    rooms[i].active          = false;
    rooms[i].next_post_idx   = 0;
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

  // Stagger initial advert timers per room to avoid collision
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    uint32_t offset_ms = (uint32_t)i * 15000;  // 15 s stagger
    rooms[i].next_local_advert = futureMillis(LOCAL_ADVERT_INTERVAL_MS + offset_ms);
    rooms[i].next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + offset_ms);
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

  if (!store.load(key, rooms[idx].id)) {
    rooms[idx].id = radio_new_identity();
    int attempts = 0;
    while (attempts < 10 &&
           (rooms[idx].id.pub_key[0] == 0x00 ||
            rooms[idx].id.pub_key[0] == 0xFF)) {
      rooms[idx].id = radio_new_identity();
      attempts++;
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
    uint8_t active = rooms[i].active ? 1 : 0;
    f.write(&active, 1);
    f.write((uint8_t*)rooms[i].name,          sizeof(rooms[i].name));
    f.write((uint8_t*)rooms[i].password,      sizeof(rooms[i].password));
    f.write((uint8_t*)rooms[i].guest_password,sizeof(rooms[i].guest_password));
    f.write((uint8_t*)&rooms[i].lat, 4);
    f.write((uint8_t*)&rooms[i].lon, 4);
  }
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

    rooms[i].active = (active != 0);
    if (rooms[i].active) _num_active_rooms++;
  }
  f.close();
}

/* ------------------------------------------------------------------ */
/*  onRecvPacket — multi-room routing override                          */
/* ------------------------------------------------------------------ */
DispatcherAction MultiRoomMesh::onRecvPacket(mesh::Packet* pkt) {
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
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i;
      if (n >= MAX_CLIENTS) break;
    }
  }
  return n;
}

void MultiRoomMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  ClientACL& acl = rooms[_active_slot].acl;
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
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
  RoomSlot& slot = rooms[_active_slot];
  int i = matching_peer_indexes[sender_idx];
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
  int idx = slot.next_post_idx;
  slot.posts[idx].author = client->id;
  StrHelper::strncpy(slot.posts[idx].text, text, MAX_POST_TEXT_LEN);
  slot.posts[idx].post_timestamp = getRTCClock()->getCurrentTimeUnique();
  slot.next_post_idx = (idx + 1) % MAX_UNSYNCED_POSTS;
  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  slot.num_posted++;
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
  uint8_t count = 0;
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (slot.posts[k].post_timestamp > client->extra.room.sync_since &&
        !slot.posts[k].author.matches(client->id)) {
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
    sendRoomAdvertisement(rooms[i], delay_millis + (uint32_t)i * 1000, flood);
  }
}

/* ------------------------------------------------------------------ */
/*  Advert timer management                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::updateAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (_prefs.advert_interval > 0) {
      rooms[i].next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000);
    } else {
      rooms[i].next_local_advert = 0;
    }
  }
}

void MultiRoomMesh::updateFloodAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    if (_prefs.flood_advert_interval > 0) {
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

    uint32_t now = getRTCClock()->getCurrentTime();
    for (int k = 0, idx = slot.next_post_idx; k < MAX_UNSYNCED_POSTS; k++) {
      PostInfo& p = slot.posts[idx];
      if (now >= p.post_timestamp + POST_SYNC_DELAY_SECS &&
          p.post_timestamp > client->extra.room.sync_since &&
          !p.author.matches(client->id)) {
        // Set self_id to this room before sending so the packet is signed correctly
        self_id = slot.id;
        pushPostToClient(slot, client, p);
        did_push = true;
        break;
      }
      idx = (idx + 1) % MAX_UNSYNCED_POSTS;
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

    // Flood advert timer
    if (slot.next_flood_advert && millisHasNowPassed(slot.next_flood_advert)) {
      self_id = slot.id;
      sendRoomAdvertisement(slot, 0, true);
      slot.next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + (uint32_t)i * 15000);
      slot.next_local_advert = futureMillis(LOCAL_ADVERT_INTERVAL_MS + (uint32_t)i * 15000);
    } else if (slot.next_local_advert && millisHasNowPassed(slot.next_local_advert)) {
      self_id = slot.id;
      sendRoomAdvertisement(slot, 0, false);
      slot.next_local_advert = futureMillis(LOCAL_ADVERT_INTERVAL_MS + (uint32_t)i * 15000);
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

  // ---- room management commands ----
  if (memcmp(command, "room ", 5) == 0) {
    handleRoomCommand(command + 5, reply);
    return;
  }

  // ---- ACL commands scoped to active_slot (or room[0] for serial) ----
  int scope = (sender_timestamp == 0) ? 0 : _active_slot;
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

  // Fall through to shared CommonCLI
  _cli.handleCommand(sender_timestamp, command, reply);
}

/* ------------------------------------------------------------------ */
/*  room * CLI sub-commands                                             */
/* ------------------------------------------------------------------ */
void MultiRoomMesh::handleRoomCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  // "room list" — list all rooms
  if (strcmp(args, "list") == 0 || strcmp(args, "ls") == 0) {
    if (_fs) {  // serial output
      Serial.printf("Rooms (%d/%d active):\n", _num_active_rooms, MAX_ROOMS);
      for (int i = 0; i < MAX_ROOMS; i++) {
        Serial.printf("  [%d] %s  name='%s'  id=", i,
                      rooms[i].active ? "ON " : "OFF", rooms[i].name);
        if (rooms[i].active) {
          mesh::Utils::printHex(Serial, rooms[i].id.pub_key, 4);
          Serial.printf("...  clients=%d  posts=%d",
                        rooms[i].acl.getNumClients(), rooms[i].num_posted);
        }
        Serial.println();
      }
      reply[0] = 0;
    } else {
      sprintf(reply, "%d rooms active", _num_active_rooms);
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

  // "room del <idx>"
  if (memcmp(args, "del ", 4) == 0) {
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

  strcpy(reply, "Err - usage: room list|add|del <idx>|set <idx> name|pass|guest <value>");
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
