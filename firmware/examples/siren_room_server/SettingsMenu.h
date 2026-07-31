#pragma once
/*
 * SettingsMenu.h — Interactive serial settings menu for SIREN room server.
 *
 * Trigger:  type  menu  + Enter in the normal serial CLI.
 * Navigate: number keys + Enter.
 * All changes are dispatched to MultiRoomMesh::handleCommand() and saved to
 * NVS (SPIFFS) exactly as the existing CLI commands do.
 *
 * No mesh-CLI compatibility is broken: handleCommand() is unchanged for
 * over-mesh (Phase 4) calls; this menu only runs on sender_timestamp == 0
 * (local serial).
 */

#include <Arduino.h>
#include "MyMesh.h"

class SettingsMenu {
  /* ------------------------------------------------------------------ */
  /*  State machine                                                       */
  /* ------------------------------------------------------------------ */
  enum State {
    IDLE,
    MAIN,
    RADIO,
    IDENTITY,
    ROOMS,
    NETWORK,
    INPUT_LINE,   // generic single-line value capture
  };

  enum Action {
    ACT_NONE,
    /* Radio */
    ACT_RADIO_FREQ,
    ACT_RADIO_BW,
    ACT_RADIO_SF,
    ACT_RADIO_CR,
    ACT_RADIO_POWER,
    /* Identity */
    ACT_NODE_NAME,
    ACT_ADMIN_PASS,
    /* Rooms — two-step: first capture room index, then value */
    ACT_ROOM_IDX_FOR_NAME,
    ACT_ROOM_IDX_FOR_PASS,
    ACT_ROOM_IDX_FOR_GUEST,
    ACT_ROOM_IDX_FOR_ADD,
    ACT_ROOM_IDX_FOR_DEL,
    ACT_ROOM_NAME,
    ACT_ROOM_PASS,
    ACT_ROOM_GUEST,
    ACT_ROOM_DEL,
    /* Network */
    ACT_NET_FLOOD_MAX,
    ACT_NET_FLOOD_MAX_UNSCOPED,
    ACT_NET_FLOOD_MAX_ADVERT,
    ACT_NET_ADVERT_INT,
  };

  MultiRoomMesh* _mesh;
  State  _state;
  Action _action;
  int    _room_idx;   // room index captured in first step of two-step room edits

  char _linebuf[64];
  int  _linelen;

  /* ------------------------------------------------------------------ */
  /*  CLI dispatch helper                                                 */
  /* ------------------------------------------------------------------ */
  void dispatch(const char* cmd) {
    char reply[160] = "";
    char mutable_cmd[160];
    strncpy(mutable_cmd, cmd, sizeof(mutable_cmd) - 1);
    mutable_cmd[sizeof(mutable_cmd) - 1] = 0;
    _mesh->handleCommand(0, mutable_cmd, reply);
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
  }

  /* ------------------------------------------------------------------ */
  /*  Menu display helpers                                                */
  /* ------------------------------------------------------------------ */
  void showMain() {
    Serial.println();
    Serial.println("=== SIREN Settings Menu ===");
    Serial.println("  1. Radio Settings");
    Serial.println("  2. Node Identity");
    Serial.println("  3. Room / Channel Settings");
    Serial.println("  4. Network Settings");
    Serial.println("  5. Show current config");
    Serial.println("  6. Save & Reboot");
    Serial.println("  0. Exit");
    Serial.print("Choice: ");
    _state = MAIN;
  }

  void showRadio() {
    char reply[160];
    char mutable_get[32];
    strcpy(mutable_get, "get radio");
    _mesh->handleCommand(0, mutable_get, reply);
    // reply format: "> freq,bw,sf,cr"
    Serial.println();
    Serial.println("--- 1. Radio Settings ---");
    Serial.printf("  Current: %s  (freq MHz, bw kHz, sf, cr)\n",
                  reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get tx");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  TX Power: %s dBm\n", reply[0] == '>' ? reply + 2 : reply);
    Serial.println();
    Serial.println("  1. Frequency  (MHz, e.g. 869.618)");
    Serial.println("  2. Bandwidth  (kHz: 7/15.6/20.8/31.25/41.7/62.5/125/250/500)");
    Serial.println("  3. Spreading Factor  (7-12)");
    Serial.println("  4. Coding Rate  (5=4/5  6=4/6  7=4/7  8=4/8)");
    Serial.println("  5. TX Power  (dBm, 2-22)");
    Serial.println("  0. Back");
    Serial.print("Choice: ");
    _state = RADIO;
  }

  void showIdentity() {
    char reply[160];
    char mutable_get[32];
    strcpy(mutable_get, "get name");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.println();
    Serial.println("--- 2. Node Identity ---");
    Serial.printf("  Node name: %s\n", reply[0] == '>' ? reply + 2 : reply);
    Serial.println("  (Note: admin password is shown only when set here)");
    Serial.println("  1. Change node name");
    Serial.println("  2. Change admin password");
    Serial.println("  0. Back");
    Serial.print("Choice: ");
    _state = IDENTITY;
  }

  void showRooms() {
    Serial.println();
    Serial.println("--- 3. Room / Channel Settings ---");
    dispatch("room list");
    Serial.println();
    Serial.println("  1. Set room name      (prompts for index then name)");
    Serial.println("  2. Set room password  (prompts for index then password)");
    Serial.println("  3. Set guest password (prompts for index then password)");
    Serial.println("  4. Add room");
    Serial.println("  5. Delete room");
    Serial.println("  0. Back");
    Serial.print("Choice: ");
    _state = ROOMS;
  }

  void showNetwork() {
    char reply[160];
    char mutable_get[32];
    Serial.println();
    Serial.println("--- 4. Network Settings ---");
    strcpy(mutable_get, "get flood.max");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  1. Flood max:           %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get flood.max.unscoped");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  2. Flood max unscoped:  %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get flood.max.advert");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  3. Flood max advert:    %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get advert.interval");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  4. Advert interval:     %s min\n", reply[0] == '>' ? reply + 2 : reply);
    Serial.println("  0. Back");
    Serial.print("Choice: ");
    _state = NETWORK;
  }

  void showConfig() {
    char reply[160];
    char mutable_get[32];
    Serial.println();
    Serial.println("=== Current Configuration ===");
    strcpy(mutable_get, "get name");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Node name:          %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get radio");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Radio (f,bw,sf,cr): %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get tx");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  TX Power:           %s dBm\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get advert.interval");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Advert interval:    %s min\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get flood.max");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Flood max:          %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get flood.max.unscoped");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Flood max unscoped: %s\n", reply[0] == '>' ? reply + 2 : reply);
    strcpy(mutable_get, "get flood.max.advert");
    _mesh->handleCommand(0, mutable_get, reply);
    Serial.printf("  Flood max advert:   %s\n", reply[0] == '>' ? reply + 2 : reply);
    Serial.println();
    dispatch("room list");
    Serial.println("=============================");
    showMain();
  }

  /* ------------------------------------------------------------------ */
  /*  Prompt for a line of text                                           */
  /* ------------------------------------------------------------------ */
  void prompt(const char* msg, Action act) {
    Serial.println();
    Serial.print("  ");
    Serial.print(msg);
    Serial.print(": ");
    _action   = act;
    _state    = INPUT_LINE;
    _linelen  = 0;
    _linebuf[0] = 0;
  }

  /* ------------------------------------------------------------------ */
  /*  Read current radio params from mesh (fills freq/bw/sf/cr)          */
  /* ------------------------------------------------------------------ */
  bool readRadio(float& freq, float& bw, int& sf, int& cr) {
    char reply[160];
    char mutable_get[32];
    strcpy(mutable_get, "get radio");
    _mesh->handleCommand(0, mutable_get, reply);
    // reply: "> 869.618,62.5,8,8"
    const char* data = (reply[0] == '>') ? reply + 2 : reply;
    char tmp[64];
    strncpy(tmp, data, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* tok = strtok(tmp, ",");
    if (!tok) return false;
    freq = strtof(tok, nullptr);
    tok = strtok(nullptr, ",");
    if (!tok) return false;
    bw = strtof(tok, nullptr);
    tok = strtok(nullptr, ",");
    if (!tok) return false;
    sf = atoi(tok);
    tok = strtok(nullptr, ",");
    if (!tok) return false;
    cr = atoi(tok);
    return true;
  }

  /* ------------------------------------------------------------------ */
  /*  Process a completed input line                                      */
  /* ------------------------------------------------------------------ */
  void handleInput(const char* value) {
    char cmd[128];

    switch (_action) {

      /* ---- Radio ---- */
      case ACT_RADIO_FREQ: {
        float freq = strtof(value, nullptr);
        if (freq < 150.0f || freq > 2500.0f) {
          Serial.println("  Error: frequency must be 150-2500 MHz");
        } else {
          float ofreq, bw; int sf, cr;
          if (readRadio(ofreq, bw, sf, cr)) {
            snprintf(cmd, sizeof(cmd), "set radio %.3f %.3f %d %d", freq, bw, sf, cr);
            dispatch(cmd);
          }
        }
        showRadio();
        break;
      }
      case ACT_RADIO_BW: {
        float bw = strtof(value, nullptr);
        if (bw < 7.0f || bw > 500.0f) {
          Serial.println("  Error: bandwidth out of range");
        } else {
          float freq, obw; int sf, cr;
          if (readRadio(freq, obw, sf, cr)) {
            snprintf(cmd, sizeof(cmd), "set radio %.3f %.3f %d %d", freq, bw, sf, cr);
            dispatch(cmd);
          }
        }
        showRadio();
        break;
      }
      case ACT_RADIO_SF: {
        int sf = atoi(value);
        if (sf < 7 || sf > 12) {
          Serial.println("  Error: SF must be 7-12");
        } else {
          float freq, bw; int osf, cr;
          if (readRadio(freq, bw, osf, cr)) {
            snprintf(cmd, sizeof(cmd), "set radio %.3f %.3f %d %d", freq, bw, sf, cr);
            dispatch(cmd);
          }
        }
        showRadio();
        break;
      }
      case ACT_RADIO_CR: {
        int cr = atoi(value);
        if (cr < 5 || cr > 8) {
          Serial.println("  Error: CR must be 5-8 (5=4/5, 6=4/6, 7=4/7, 8=4/8)");
        } else {
          float freq, bw; int sf, ocr;
          if (readRadio(freq, bw, sf, ocr)) {
            snprintf(cmd, sizeof(cmd), "set radio %.3f %.3f %d %d", freq, bw, sf, cr);
            dispatch(cmd);
          }
        }
        showRadio();
        break;
      }
      case ACT_RADIO_POWER: {
        int pwr = atoi(value);
        if (pwr < 2 || pwr > 22) {
          Serial.println("  Error: TX power must be 2-22 dBm");
        } else {
          snprintf(cmd, sizeof(cmd), "set txpower %d", pwr);
          dispatch(cmd);
        }
        showRadio();
        break;
      }

      /* ---- Identity ---- */
      case ACT_NODE_NAME:
        snprintf(cmd, sizeof(cmd), "set name %s", value);
        dispatch(cmd);
        showIdentity();
        break;

      case ACT_ADMIN_PASS:
        snprintf(cmd, sizeof(cmd), "password %s", value);
        dispatch(cmd);
        showIdentity();
        break;

      /* ---- Rooms: two-step (index → field) ---- */
      case ACT_ROOM_IDX_FOR_NAME:
        _room_idx = atoi(value);
        prompt("New room name", ACT_ROOM_NAME);
        break;

      case ACT_ROOM_IDX_FOR_PASS:
        _room_idx = atoi(value);
        prompt("New room password", ACT_ROOM_PASS);
        break;

      case ACT_ROOM_IDX_FOR_GUEST:
        _room_idx = atoi(value);
        prompt("New guest password (empty = no password)", ACT_ROOM_GUEST);
        break;

      case ACT_ROOM_IDX_FOR_DEL:
        _room_idx = atoi(value);
        snprintf(cmd, sizeof(cmd), "room del %d", _room_idx);
        dispatch(cmd);
        showRooms();
        break;

      case ACT_ROOM_NAME:
        snprintf(cmd, sizeof(cmd), "room set %d name %s", _room_idx, value);
        dispatch(cmd);
        showRooms();
        break;

      case ACT_ROOM_PASS:
        snprintf(cmd, sizeof(cmd), "room set %d pass %s", _room_idx, value);
        dispatch(cmd);
        showRooms();
        break;

      case ACT_ROOM_GUEST:
        snprintf(cmd, sizeof(cmd), "room set %d guest %s", _room_idx, value);
        dispatch(cmd);
        showRooms();
        break;

      /* ---- Network ---- */
      case ACT_NET_FLOOD_MAX:
        snprintf(cmd, sizeof(cmd), "set flood.max %s", value);
        dispatch(cmd);
        showNetwork();
        break;

      case ACT_NET_FLOOD_MAX_UNSCOPED:
        snprintf(cmd, sizeof(cmd), "set flood.max.unscoped %s", value);
        dispatch(cmd);
        showNetwork();
        break;

      case ACT_NET_FLOOD_MAX_ADVERT:
        snprintf(cmd, sizeof(cmd), "set flood.max.advert %s", value);
        dispatch(cmd);
        showNetwork();
        break;

      case ACT_NET_ADVERT_INT:
        snprintf(cmd, sizeof(cmd), "set advert.interval %s", value);
        dispatch(cmd);
        showNetwork();
        break;

      default:
        showMain();
        break;
    }
  }

  /* ------------------------------------------------------------------ */
  /*  Main-menu key dispatch                                              */
  /* ------------------------------------------------------------------ */
  void dispatchMain(char key) {
    switch (key) {
      case '1': showRadio();    break;
      case '2': showIdentity(); break;
      case '3': showRooms();    break;
      case '4': showNetwork();  break;
      case '5': showConfig();   break;
      case '6':
        Serial.println("  Saving and rebooting...");
        dispatch("reboot");
        break;
      case '0':
        Serial.println("  Exiting settings menu.");
        _state = IDLE;
        break;
      default:
        Serial.println("  Invalid choice.");
        showMain();
        break;
    }
  }

  void dispatchRadio(char key) {
    switch (key) {
      case '1': prompt("Frequency (MHz, e.g. 869.618)",                    ACT_RADIO_FREQ);  break;
      case '2': prompt("Bandwidth (kHz, e.g. 62.5)",                       ACT_RADIO_BW);    break;
      case '3': prompt("Spreading Factor (7-12)",                          ACT_RADIO_SF);    break;
      case '4': prompt("Coding Rate (5=4/5 6=4/6 7=4/7 8=4/8)",           ACT_RADIO_CR);    break;
      case '5': prompt("TX Power dBm (2-22, SX1262 max=22)",               ACT_RADIO_POWER); break;
      case '0': showMain(); break;
      default:  Serial.println("  Invalid choice."); showRadio(); break;
    }
  }

  void dispatchIdentity(char key) {
    switch (key) {
      case '1': prompt("Node name (max 31 chars)", ACT_NODE_NAME);  break;
      case '2': prompt("Admin password (max 15 chars)", ACT_ADMIN_PASS); break;
      case '0': showMain(); break;
      default:  Serial.println("  Invalid choice."); showIdentity(); break;
    }
  }

  void dispatchRooms(char key) {
    switch (key) {
      case '1': prompt("Room index to edit name",     ACT_ROOM_IDX_FOR_NAME);  break;
      case '2': prompt("Room index to edit password", ACT_ROOM_IDX_FOR_PASS);  break;
      case '3': prompt("Room index to edit guest pw", ACT_ROOM_IDX_FOR_GUEST); break;
      case '4': dispatch("room add"); showRooms(); break;
      case '5': prompt("Room index to delete (0 not allowed)", ACT_ROOM_IDX_FOR_DEL); break;
      case '0': showMain(); break;
      default:  Serial.println("  Invalid choice."); showRooms(); break;
    }
  }

  void dispatchNetwork(char key) {
    switch (key) {
      case '1': prompt("Flood max (1-64)",          ACT_NET_FLOOD_MAX);           break;
      case '2': prompt("Flood max unscoped (1-64)", ACT_NET_FLOOD_MAX_UNSCOPED);  break;
      case '3': prompt("Flood max advert (1-64)",   ACT_NET_FLOOD_MAX_ADVERT);    break;
      case '4': prompt("Advert interval (minutes)", ACT_NET_ADVERT_INT);          break;
      case '0': showMain(); break;
      default:  Serial.println("  Invalid choice."); showNetwork(); break;
    }
  }

public:
  explicit SettingsMenu(MultiRoomMesh& mesh)
    : _mesh(&mesh), _state(IDLE), _action(ACT_NONE),
      _room_idx(0), _linelen(0)
  {
    _linebuf[0] = 0;
  }

  bool isActive() const { return _state != IDLE; }

  void enter() { showMain(); }

  /* Feed one character from Serial; call this for every byte while isActive(). */
  void feed(char c) {
    /* Echo printable characters while collecting input */
    if (c >= 0x20 && c < 0x7F) {
      if (_linelen < (int)(sizeof(_linebuf) - 1)) {
        _linebuf[_linelen++] = c;
        _linebuf[_linelen]   = 0;
        Serial.print(c);
      }
      return;
    }

    /* Backspace */
    if ((c == 0x08 || c == 0x7F) && _linelen > 0) {
      _linelen--;
      _linebuf[_linelen] = 0;
      Serial.print("\b \b");
      return;
    }

    /* CR or LF → process line */
    if (c == '\r' || c == '\n') {
      Serial.println();
      const char* value = _linebuf;

      if (_state == INPUT_LINE) {
        handleInput(value);
        _linelen = 0;
        _linebuf[0] = 0;
        return;
      }

      /* Single-key menu dispatch: use first char */
      char key = (_linelen > 0) ? _linebuf[0] : 0;
      _linelen = 0;
      _linebuf[0] = 0;

      switch (_state) {
        case MAIN:     dispatchMain(key);     break;
        case RADIO:    dispatchRadio(key);    break;
        case IDENTITY: dispatchIdentity(key); break;
        case ROOMS:    dispatchRooms(key);    break;
        case NETWORK:  dispatchNetwork(key);  break;
        default: break;
      }
    }
  }
};
