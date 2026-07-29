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
  #define FIRMWARE_BUILD_DATE  "2026"
#endif
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION     "v1.0-siren-p1"
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
#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD       "password"
#endif
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

#define FIRMWARE_ROLE        "siren_room"
#define MAX_POST_TEXT_LEN    (160 - 9)

/* ------------------------------------------------------------------ */
/*  Data types                                                          */
/* ------------------------------------------------------------------ */

struct PostInfo {
  mesh::Identity  author;
  uint32_t        post_timestamp;
  char            text[MAX_POST_TEXT_LEN + 1];
  uint8_t         room_idx;     // owning room (0xFF = free slot)
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

  unsigned long next_push;
  unsigned long next_local_advert;
  unsigned long next_flood_advert;
  unsigned long dirty_contacts_expiry;
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

  /* ---- Shared mesh state ---- */
  uint32_t      last_millis;
  uint64_t      uptime_millis;
  bool          _logging;
  bool          region_load_active;

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

  /* ---- CLI helpers ---- */
  void          handleRoomCommand(char* args, char* reply);
  int           handleRequest(RoomSlot& slot, ClientInfo* sender,
                              uint32_t sender_timestamp,
                              uint8_t* payload, size_t payload_len);

protected:
  /* ---- Override packet dispatch for multi-room routing ---- */
  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

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
};
