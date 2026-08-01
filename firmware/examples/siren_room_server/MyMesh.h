#pragma once

#include <Arduino.h>
#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/CommonCLI.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>
#include <RTClib.h>
#include <target.h>

/* ------------------------------------------------------------------ */
/*  Build-flag defaults                                                 */
/* ------------------------------------------------------------------ */

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE  "2026-08-01"
#endif
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION     "v1.9.0"
#endif
#ifndef LORA_FREQ
  #define LORA_FREQ            869.618
#endif
#ifndef LORA_BW
  #define LORA_BW              62.5
#endif
#ifndef LORA_SF
  #define LORA_SF              8
#endif
#ifndef LORA_CR
  #define LORA_CR              5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER        20
#endif
// SEC-001: no compile-time default password.
// Admin password is randomised on first boot and persisted to SPIFFS.
// Operators may still override at build time via platformio_local.ini:
//   build_flags = ... -D ADMIN_PASSWORD='"mypassword"'
// That value is used as the initial seed; if it equals the legacy "password"
// the firmware still randomises to protect public dist/ binaries.
#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY  300
#endif
#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY          200
#endif
/* Total post budget shared across all active rooms.
   Per-room quota = MAX_TOTAL_POSTS / num_active_rooms.
   With MAX_ROOMS=16 active: 8 posts/room; with 2 active: 64 posts/room. */
#ifndef MAX_TOTAL_POSTS
  #define MAX_TOTAL_POSTS        128
#endif

/* Maximum virtual room servers hosted on one device. */
#ifndef MAX_ROOMS
  #define MAX_ROOMS  16
#endif

/* Maximum tombstones tracked for replicating post deletes (JES-824). Oldest evicted when full. */
#ifndef MAX_TOMBSTONES
  #define MAX_TOMBSTONES 64
#endif

/* Maximum login-notification targets per room (JES-834). */
#ifndef MAX_NOTIFY_TARGETS
  #define MAX_NOTIFY_TARGETS 4
#endif

/* Maximum peer room-server nodes tracked for Phase 5 replication. */
#ifndef MAX_PEERS
  #define MAX_PEERS  8
#endif

/* Phase 5: version-vector replication constants. */
#define MAX_VV_ORIGINS  8    // per-room VV entries (one per known origin server)
#define MAX_SYNC_POSTS  8    // SYNCDAT frames per SYNCREQ response (airtime guard)
/* Periodic anti-entropy pull interval. 3-min default; boot delay is 45 s per peer. */
#define PEER_SYNC_INTERVAL_MS   (3UL * 60 * 1000)
#define PEER_SYNC_BOOT_DELAY_MS 45000UL

/* Server-to-server sync TXT sub-types (data[4] >> 2).
   Range 4-6 is free above the existing SIGNED_PLAIN=2, CLI_DATA=1, PLAIN=0. */
#define TXT_TYPE_SYNCREQ  4   // A→B pull request  [ts][flags][num_vv][VV...]
#define TXT_TYPE_SYNCDAT  5   // B→A one post       [ts][flags][post_ts][orig[4]][auth[4]][text]
#define TXT_TYPE_SYNCEND  6   // B→A end of stream  [ts][flags][num_vv][VV...]
#define TXT_TYPE_SYNCDEL  7   // delete tombstone   [ts][flags][room_hash[4]][origin_id[4]][post_ts[4]]

/* Name resolution table — maps pubkey prefix → advertised node name.
   Populated from onAdvertRecv(); persisted to SPIFFS /names.          */
#define NAME_TABLE_SIZE 32
#define NAME_KEY_SIZE    4   // first 4 bytes of pubkey used as lookup key

/* DM (direct message) ring-buffer — web-UI admin ↔ companion node.
   Capacity: DM_MAX_CONVS contacts × DM_MAX_MSGS messages each.
   Total RAM: ~8×6×160 = ~7.7 KB (acceptable on no-PSRAM Heltec). */
#define DM_MAX_CONVS  8    // max simultaneous DM conversations tracked
#define DM_MAX_MSGS   6    // messages per conversation (ring buffer)
#define DM_TEXT_LEN   (MAX_POST_TEXT_LEN + 1)

#define FIRMWARE_ROLE        "siren_room"
#define MAX_POST_TEXT_LEN    (160 - 9)

/* ------------------------------------------------------------------ */
/*  Data types                                                          */
/* ------------------------------------------------------------------ */

/**
 * Version-vector entry: highest post_timestamp seen from a given room-server origin.
 * Used for Phase 5 anti-entropy replication.
 */
struct VVEntry {
  uint8_t  origin_id[4];  // 4-byte prefix of originating room-server pubkey
  uint32_t seq;           // highest post_timestamp from that origin (0 = unknown)
};

/**
 * A known peer room-server node (for Phase 5 anti-entropy replication).
 * PERSISTED fields (active/name/pub_key/last_contact) stored in /peer_cfg.
 * RUNTIME fields (shared_secret/secret_valid/next_sync_at) recomputed each boot.
 */
struct PeerInfo {
  /* PERSISTED */
  bool     active;
  char     name[24];
  uint8_t  pub_key[PUB_KEY_SIZE];  // 32 bytes
  uint32_t last_contact;           // RTC timestamp of last packet; 0 = never
  /* RUNTIME — not saved */
  uint8_t  shared_secret[PUB_KEY_SIZE]; // ECDH(rooms[0].priv, pub_key)
  bool     secret_valid;                // true once calcPeerSecret() called
  unsigned long next_sync_at;           // millis() deadline for next SYNCREQ
  /* SYNC DIAGNOSTICS (JES-833) — not persisted, reset on reboot */
  uint32_t last_syncreq_ts;  // RTC epoch of last SYNCREQ sent to this peer (0 = never)
  uint32_t last_syncdat_ts;  // RTC epoch of last SYNCDAT received from this peer (0 = never)
  uint32_t last_syncend_ts;  // RTC epoch of last SYNCEND received from this peer (0 = never)
  uint32_t sync_posts_recv;  // posts received from this peer via sync (since boot)
  uint32_t sync_posts_sent;  // posts sent to this peer via sync (since boot)
};

struct NameEntry {
  uint8_t  pub_prefix[NAME_KEY_SIZE];   // first 4 bytes of pubkey
  char     name[24];                    // advertised name (NUL-terminated)
  uint32_t lru_seq;                     // 0 = empty; higher = more recently seen
};

/** One message in a DM conversation (bidirectional). */
struct DmMsg {
  uint32_t ts;            // RTC timestamp
  bool     outgoing;      // true = server→companion, false = companion→server
  char     text[DM_TEXT_LEN];
};

/** DM conversation with one companion (ring buffer of last DM_MAX_MSGS messages). */
struct DmConv {
  uint8_t  pub_prefix[NAME_KEY_SIZE];  // first 4 bytes of companion's pubkey
  DmMsg    msgs[DM_MAX_MSGS];
  uint8_t  head;    // next-write index (ring)
  uint8_t  count;   // messages stored (0..DM_MAX_MSGS)
};

struct PostInfo {
  mesh::Identity  author;
  uint32_t        post_timestamp;
  char            text[MAX_POST_TEXT_LEN + 1];
  uint8_t         room_idx;       // owning room (0xFF = free slot)
  uint8_t         origin_id[4];   // Phase 5: 4-byte prefix of room-server that originated post
};

/**
 * Tombstone for a deleted post (JES-824). Keyed on (origin_id, post_ts).
 * room_hash (first 4 bytes of room pub_key) stored for SYNCDEL routing during SYNCREQ.
 */
struct Tombstone {
  uint8_t  origin_id[4];
  uint32_t post_ts;
  uint8_t  room_hash[4];  // first 4 bytes of the room's pub_key
};

/**
 * One virtual room server: owns its own keypair, name, passwords,
 * client ACL, and post ring-buffer.
 */
struct RoomSlot {
  bool              active;
  mesh::LocalIdentity id;

  char  name[24];           // advertised room name
  char  password[16];       // admin password
  char  guest_password[16]; // room access password (empty = no password required)
  float lat;
  float lon;

  ClientACL  acl;
  int        next_client_idx;
  uint16_t   num_posted;
  uint16_t   num_post_pushes;

  bool          stealth;           // if true: no adverts/location sent (default)

  unsigned long next_push;
  unsigned long next_local_advert;
  unsigned long next_flood_advert;
  unsigned long dirty_contacts_expiry;

  /* Phase 5: per-room version vector (one entry per known origin room-server). */
  VVEntry       vv[MAX_VV_ORIGINS];
};

/* ------------------------------------------------------------------ */
/*  MultiRoomMesh                                                       */
/* ------------------------------------------------------------------ */

class MultiRoomMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM*   _fs;

  /* ---- Room slots ---- */
  RoomSlot      rooms[MAX_ROOMS];
  int           _num_active_rooms;
  int           _active_slot;     // set during onRecvPacket dispatch

  /* ---- Global post pool (shared budget across all rooms) ---- */
  PostInfo      _post_pool[MAX_TOTAL_POSTS];

  /* ---- Peer room-server list (Phase 5 replication ground work) ---- */
  PeerInfo      peers[MAX_PEERS];
  int           _num_peers;

  /* ---- Shared mesh state ---- */
  uint32_t      last_millis;
  uint64_t      uptime_millis;
  bool          _logging;
  bool          region_load_active;
  uint16_t      _advert_interval_sec;  // local advert period in seconds (10-3600, default 120)

  NodePrefs         _prefs;         // radio / mesh settings (shared)
  TransportKeyStore key_store;
  RegionMap         region_map, temp_map;

  /* Room 0's ACL is lent to CommonCLI for 'setperm'/'get acl' commands */
  CommonCLI         _cli;

  uint8_t       reply_data[MAX_PACKET_PAYLOAD];
  int           matching_peer_indexes[MAX_CLIENTS];

  CayenneLPP    telemetry;
  RegionEntry*  load_stack[8];
  RegionEntry*  recv_pkt_region;
  TransportKey  default_scope;

  unsigned long set_radio_at, revert_radio_at;
  float         pending_freq, pending_bw;
  uint8_t       pending_sf, pending_cr;

  /* ---- Post-pool dirty timer (JES-794) ---- */
  unsigned long _post_dirty_at;   // 0 = not dirty; set to futureMillis(5000) on new post

  /* ---- Tombstone log (JES-824) ---- */
  Tombstone _tombstones[MAX_TOMBSTONES];
  uint8_t   _tombstone_count;

  /* ---- Sync diagnostics counters (JES-833) — RAM only, reset on reboot ---- */
  uint32_t  _sync_req_sent;    // total SYNCREQ frames sent
  uint32_t  _sync_dat_recv;    // total SYNCDAT frames received
  uint32_t  _sync_posts_recv;  // total posts ingested via sync
  uint32_t  _sync_posts_sent;  // total posts pushed to peers

  /* ---- Login notification targets + rate-limit (JES-834) ---- */
  uint32_t  _last_login_notify_ms[MAX_ROOMS];  // millis() of last DM sent per room
  uint8_t   _notify_targets[MAX_ROOMS][MAX_NOTIFY_TARGETS][PUB_KEY_SIZE];
  uint8_t   _notify_target_count[MAX_ROOMS];

  /* ---- Message-rate histogram ring-buffer (JES-800) ---- */
  /* 24 buckets × 1 hour = rolling 24-hour window. RAM: 24×2 = 48 bytes. */
  #define HIST_BUCKETS 24
  uint16_t  _hist_ring[HIST_BUCKETS];   // messages in each 1-hour bucket
  uint8_t   _hist_head;                 // current (write) bucket index
  uint32_t  _hist_bucket_ts;            // RTC timestamp when _hist_head bucket started

  void saveTombstones();
  void loadTombstones();
  bool isTombstoned(const uint8_t* origin_id, uint32_t post_ts);
  void addTombstone(const uint8_t* origin_id, uint32_t post_ts, const uint8_t* room_hash);
  bool deletePostEntry(uint8_t room_idx, const uint8_t* origin_id, uint32_t post_ts);
  void emitSyncDel(const uint8_t* room_hash, const uint8_t* origin_id, uint32_t post_ts);
  void handleSyncDel(int pi, uint8_t* data, size_t len);

  /* ---- Name resolution table (JES-798) ---- */
  NameEntry     _names[NAME_TABLE_SIZE];
  uint32_t      _name_lru_ctr;

  void          saveNameTable();
  void          loadNameTable();

  /* ---- DM ring buffer (JES-808) ---- */
  DmConv        _dm_convs[DM_MAX_CONVS];
  int           _dm_num_convs;

  /** Buffer one DM message (incoming or outgoing) into the per-contact ring. */
  void          dmBuffer(const uint8_t* pub_prefix, uint32_t ts, bool outgoing, const char* text);

  /* ---- MQTT publish callback (JES-792) ---- */
  typedef void (*PostPublishCallback)(int room_idx, uint32_t timestamp,
                                      const uint8_t* author_pub,
                                      const char* text, void* ctx);
  PostPublishCallback _mqtt_post_cb;
  void*               _mqtt_post_ctx;

  /* ---- Per-slot helpers ---- */
  void          addPost(RoomSlot& slot, ClientInfo* client, const char* text);
  void          pushPostToClient(RoomSlot& slot, ClientInfo* client, PostInfo& post);
  uint8_t       getUnsyncedCount(RoomSlot& slot, ClientInfo* client);
  bool          processAckForSlot(RoomSlot& slot, const uint8_t* data);
  mesh::Packet* createRoomAdvert(RoomSlot& slot);
  void          sendRoomAdvertisement(RoomSlot& slot, int delay_millis, bool flood);
  void          loopSlot(RoomSlot& slot);

  /* ---- Identity persistence ---- */
  void          saveRoomIdentity(int idx);
  void          loadOrCreateRoomIdentity(int idx);
  void          saveRoomConfig();
  void          loadRoomConfig();

  /* ---- Peer persistence (Phase 5 replication) ---- */
  void          savePeerConfig();
  void          loadPeerConfig();

  /* ---- Phase 5 anti-entropy helpers ---- */
  void          calcPeerSecret(int pi);
  bool          vvUpdate(RoomSlot& slot, const uint8_t* origin_id, uint32_t ts);
  void          sendSyncReq(int pi);
  void          handleSyncReq(int pi, uint8_t* data, size_t len);
  void          handleSyncDat(int pi, uint8_t* data, size_t len);
  void          handleSyncEnd(int pi, uint8_t* data, size_t len);
  void          pushPostToPeer(int pi, RoomSlot& slot, PostInfo& post);
  bool          ingestSyncPost(uint8_t ridx, const uint8_t* origin_id,
                               uint32_t ts, const uint8_t* author_pub, const char* text);

  /* ---- Post pool persistence (JES-787) ---- */
  void          savePostPool();
  void          loadPostPool();

  /* ---- Login-attempt admin notification (JES-834) ---- */
  void          _notifyAdminsLoginAttempt(int slot_idx, const uint8_t* caller_pubkey, bool success);

  void          saveNotifyTargets();
  void          loadNotifyTargets();

  /* ---- CLI helpers ---- */
  void          handleRoomCommand(char* args, char* reply, bool serial);
  void          handlePeerCommand(char* args, char* reply, bool serial);
  int           handleRequest(RoomSlot& slot, ClientInfo* sender,
                              uint32_t sender_timestamp,
                              uint8_t* payload, size_t payload_len);

protected:
  /* ---- Override packet dispatch for multi-room routing ---- */
  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

  /* ---- Advert receive: populate name table ---- */
  void onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id, uint32_t ts,
                    const uint8_t* app_data, size_t app_data_len) override;

  /* ---- Tuning overrides (unchanged from simple_room_server) ---- */
  float    getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int      calcRxDelay(float score, uint32_t air_time) const override;
  const char* getLogDateTime() override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;
  int      getInterferenceThreshold() const override { return _prefs.interference_threshold; }
  int      getAGCResetInterval() const override { return (int)_prefs.agc_reset_interval * 4000; }
  uint8_t  getExtraAckTransmitCount() const override { return _prefs.multi_acks; }

  bool filterRecvFloodPacket(mesh::Packet* pkt) override;
  bool allowPacketForward(const mesh::Packet* packet) override;

  /* ---- Mesh callbacks — all use _active_slot for room context ---- */
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                      const mesh::Identity& sender,
                      uint8_t* data, size_t len) override;
  int  searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                      const uint8_t* secret,
                      uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx,
                      const uint8_t* secret,
                      uint8_t* path, uint8_t path_len,
                      uint8_t extra_type, uint8_t* extra,
                      uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override;

  void sendFloodReply(mesh::Packet* packet,
                      unsigned long delay_millis,
                      uint8_t path_hash_size);

public:
  MultiRoomMesh(mesh::MainBoard& board, mesh::Radio& radio,
                mesh::MillisecondClock& ms, mesh::RNG& rng,
                mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);

  /* ---- CommonCLICallbacks ---- */
  const char*          getFirmwareVer()  override { return FIRMWARE_VERSION; }
  const char*          getBuildDate()    override { return FIRMWARE_BUILD_DATE; }
  const char*          getRole()         override { return FIRMWARE_ROLE; }
  const char*          getNodeName()              { return _prefs.node_name; }
  NodePrefs*           getNodePrefs()             { return &_prefs; }
  void                 savePrefs()       override { _cli.savePrefs(_fs); }
  mesh::LocalIdentity& getSelfId()       override { return rooms[0].id; }
  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer()      override;
  void updateFloodAdvertTimer() override;
  void setLoggingOn(bool enable) override { _logging = enable; }
  void eraseLogFile()  override { }
  void dumpLogFile()   override { }
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr,
                             int timeout_mins) override;
  bool formatFileSystem() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char* reply) override { strcpy(reply, "not supported"); }
  void formatStatsReply(char* reply) override;
  void formatRadioStatsReply(char* reply) override;
  void formatPacketStatsReply(char* reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt,
                       uint32_t delay_millis, uint8_t path_hash_size);

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();

  /* ---- Security: runtime admin password (SEC-001) ---- */
  /** Returns the current admin password from _prefs (randomised on first boot). */
  const char* getAdminPassword() const { return _prefs.password; }

  /* ---- Public accessors for web management UI (Phase 9) ---- */
  int  getNumActiveRooms() const  { return _num_active_rooms; }
  bool isRoomActive(int i) const  { return (i >= 0 && i < MAX_ROOMS) && rooms[i].active; }
  const char* getRoomName(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? rooms[i].name : "";
  }
  int  getRoomClientCount(int i) const {
    return (i >= 0 && i < MAX_ROOMS && rooms[i].active)
           ? rooms[i].acl.getNumClients() : 0;
  }
  int  getRoomPostCount(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? (int)rooms[i].num_posted : 0;
  }
  bool isRoomStealth(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? rooms[i].stealth : true;
  }
  /** Set stealth for one room and persist; idx -1 = all rooms. */
  void setRoomStealth(int idx, bool s);

  /* Notify target management (JES-834) — public for WebManager access */
  bool          addNotifyTarget(int room_idx, const uint8_t* pub_key);
  bool          delNotifyTarget(int room_idx, const uint8_t* pub_key);
  int           getNotifyTargetCount(int room_idx) const;
  const uint8_t* getNotifyTarget(int room_idx, int i) const;

  /** Get/set local advert interval in seconds (10-3600). Persisted to SPIFFS. */
  uint16_t getAdvertIntervalSec() const { return _advert_interval_sec; }
  void     setAdvertIntervalSec(uint16_t sec);

  /** Return pointer to room i's 32-byte Ed25519 public key (PUB_KEY_SIZE bytes). */
  const uint8_t* getRoomPubKey(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? rooms[i].id.pub_key : nullptr;
  }

  /** JES-821: Generate a new private key for room idx and persist.
   *  Room 0 also updates self_id and invalidates peer ECDH secrets.
   *  Serial CLI and web admin only — never expose the new private key. */
  void rekeyRoom(int idx);

  /** JES-824: Delete post + record tombstone + persist + emit SYNCDEL to peers.
   *  Tombstone is always recorded (prevents resurrection); returns true if the
   *  post was actually found and removed from the local pool. */
  bool handleDeletePost(uint8_t room_idx, const uint8_t* origin_id, uint32_t post_ts);

  /* ---- Screensaver stats accessors (JES-781) ---- */
  uint64_t getUptimeMillis() const { return uptime_millis; }
  uint32_t getTotalPosts() const {
    uint32_t n = 0;
    for (int i = 0; i < MAX_ROOMS; i++)
      if (rooms[i].active) n += rooms[i].num_posted;
    return n;
  }
  uint8_t getTotalContacts() const {
    uint8_t n = 0;
    for (int i = 0; i < MAX_ROOMS; i++)
      if (rooms[i].active) {
        int c = rooms[i].acl.getNumClients();
        n += (c > 0) ? (uint8_t)c : 0;
      }
    return n;
  }

  /* ---- Sync diagnostics accessors (JES-833) ---- */
  uint32_t getSyncReqSent()   const { return _sync_req_sent; }
  uint32_t getSyncDatRecv()   const { return _sync_dat_recv; }
  uint32_t getSyncPostsRecv() const { return _sync_posts_recv; }
  uint32_t getSyncPostsSent() const { return _sync_posts_sent; }

  /* ---- Message-rate histogram accessors (JES-800) ---- */
  /** Returns message count for histogram bucket idx (0=current, 1=1h ago, …, 23=23h ago). */
  uint16_t getHistBucket(int idx) const {
    if (idx < 0 || idx >= HIST_BUCKETS) return 0;
    int i = ((int)_hist_head - idx + HIST_BUCKETS) % HIST_BUCKETS;
    return _hist_ring[i];
  }
  /** Advance histogram to the correct bucket for the current RTC time. */
  void histAdvance(uint32_t now_ts);

  /* ---- Post pool backup / restore (JES-790) ---- */
  /** Serialise active posts as flat JSON key-value pairs for inclusion in backup. */
  String getPostsFlatJson() const;
  /** Parse flat post key-value pairs from a full backup JSON and restore the pool. */
  bool   restorePostsFlatJson(const String& backup_json);

  /* ---- MQTT publish callback (JES-792 Phase a) ---- */
  /** Register the post-publish callback (called once from MqttManager::begin()). */
  void setPostPublishCallback(PostPublishCallback cb, void* ctx) {
    _mqtt_post_cb  = cb;
    _mqtt_post_ctx = ctx;
  }

  /* ---- IRC / chat accessors (JES-798) ---- */
  /** Resolve a 32-byte pubkey to an advertised name. Falls back to 8-char hex prefix. */
  const char* resolveName(const uint8_t* pubkey);
  /** Direct access to the global post pool for web/CLI inspection. */
  const PostInfo* getPostPool() const { return _post_pool; }
  /** Post a server-authored message to a room. Pushes to connected companions. */
  void addServerPost(int room_idx, const char* text);
  /** Build JSON array of nick objects for /api/chat/nicks. */
  String buildNickJson(int room_idx);

  /* ---- DM API (JES-808) ---- */
  /** Send a DM to the companion identified by 8-char hex pubkey prefix.
   *  Finds the client in any active room and sends via mesh.
   *  Returns true if the client was found and the packet was queued. */
  bool   dmSend(const char* pub_hex, const char* text);
  /** JSON array of active DM conversations: [{pub,name,last},...]. */
  String buildDmConvsJson();
  /** JSON array of DM thread messages for the contact with the given 8-char hex prefix. */
  String buildDmThreadJson(const char* pub_hex);

  /* ---- ACL management API for web UI (JES-720) ---- */
  /** Number of clients in room i (0 if inactive). */
  int  getRoomNumClients(int i) const {
    return (i >= 0 && i < MAX_ROOMS && rooms[i].active)
           ? rooms[i].acl.getNumClients() : 0;
  }
  /** Client at index j in room i's ACL (nullptr if out of range). */
  const ClientInfo* getRoomClient(int room, int j) {
    if (room < 0 || room >= MAX_ROOMS || !rooms[room].active) return nullptr;
    if (j < 0 || j >= rooms[room].acl.getNumClients()) return nullptr;
    return rooms[room].acl.getClientByIdx(j);
  }
  /** Set permissions for a client identified by 8-char hex pubkey prefix.
   *  Searches all clients in room; returns true on success, false if not found. */
  bool setRoomClientPerm(int room, const char* pub_hex8, uint8_t perms);

  /* ---- Peer management API for web UI (JES-816) ---- */
  int          getNumPeers() const { return _num_peers; }
  const PeerInfo* getPeer(int i) const {
    return (i >= 0 && i < MAX_PEERS) ? &peers[i] : nullptr;
  }
  /** Add a peer by full pub_key + name from web UI (admin auth enforced by caller).
   *  Returns peer index on success, or -1 on error (full / duplicate). */
  int  addPeerFromWeb(const uint8_t* pub_key, const char* name);
  /** Remove peer by index. Returns true on success. */
  bool delPeerFromWeb(int idx);
  /** Trigger immediate SYNCREQ: idx >= 0 = one peer, idx == -1 = all peers. */
  void triggerPeerSync(int idx);

  /* ---- Backup / restore accessors (JES-766) ---- */
  const char* getRoomPassword(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? rooms[i].password : "";
  }
  const char* getRoomGuestPassword(int i) const {
    return (i >= 0 && i < MAX_ROOMS) ? rooms[i].guest_password : "";
  }
  /**
   * Serialise room[i] LocalIdentity to buf (prv_key[64] || pub_key[32] = 96 bytes).
   * Returns bytes written, or 0 on error.
   */
  size_t getRoomIdentityBytes(int i, uint8_t* buf, size_t max) {
    if (i < 0 || i >= MAX_ROOMS) return 0;
    return rooms[i].id.writeTo(buf, max);
  }
  /**
   * Restore room[i] LocalIdentity from raw bytes and persist to SPIFFS.
   * Activates the slot if it was inactive.
   */
  void setRoomIdentityFromBytes(int i, const uint8_t* buf, size_t len) {
    if (i < 0 || i >= MAX_ROOMS) return;
    rooms[i].id.readFrom(buf, len);
    saveRoomIdentity(i);
  }
  /**
   * Activate room slot i (if not already active) and save config.
   * Used during restore to enable rooms that were in the backup.
   */
  void activateRoom(int i, const char* name, const char* pass, const char* guest) {
    if (i < 0 || i >= MAX_ROOMS) return;
    if (!rooms[i].active) {
      rooms[i].active = true;
      _num_active_rooms++;
    }
    if (name && name[0]) StrHelper::strncpy(rooms[i].name, name, sizeof(rooms[i].name));
    if (pass)  StrHelper::strncpy(rooms[i].password,       pass,  sizeof(rooms[i].password));
    if (guest) StrHelper::strncpy(rooms[i].guest_password, guest, sizeof(rooms[i].guest_password));
    saveRoomConfig();
  }
  /** Deactivate room slot i (i>0 only) and save config. */
  void deactivateRoom(int i) {
    if (i <= 0 || i >= MAX_ROOMS) return;
    if (rooms[i].active) {
      rooms[i].active = false;
      _num_active_rooms--;
      saveRoomConfig();
    }
  }
};
