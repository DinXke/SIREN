// BLE Codec: encode/decode messages
// Ported from web-client/siren-standalone.html

import {
  CMD_APP_START,
  CMD_DEVICE_QUERY,
  CMD_GET_CONTACTS,
  CMD_GET_CHANNEL,
  CMD_SEND_TXT_MSG,
  CMD_SEND_CHANNEL_TXT_MSG,
  CMD_SYNC_NEXT_MESSAGE,
  CMD_SEND_LOGIN,
  PUB_KEY_SIZE,
  MAX_TEXT_LEN,
  TXT_TYPE_PLAIN,
  ContactInfo,
  ChannelInfo,
  TextMessage,
} from './types';

function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join('');
}

function hexToBytes(hex: string): Uint8Array {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < hex.length; i += 2) {
    bytes[i / 2] = parseInt(hex.substr(i, 2), 16);
  }
  return bytes;
}

function writeUint32LE(view: Uint8Array, offset: number, value: number): void {
  view[offset] = value & 0xff;
  view[offset + 1] = (value >> 8) & 0xff;
  view[offset + 2] = (value >> 16) & 0xff;
  view[offset + 3] = (value >> 24) & 0xff;
}

function readUint32LE(view: Uint8Array, offset: number): number {
  return (
    view[offset] |
    (view[offset + 1] << 8) |
    (view[offset + 2] << 16) |
    (view[offset + 3] << 24)
  );
}

function readString(view: Uint8Array, offset: number, length: number): string {
  if (offset + length > view.length) return '';
  // Find null terminator
  let end = offset;
  while (end < offset + length && view[end] !== 0) {
    end++;
  }
  const bytes = view.slice(offset, end);
  return new TextDecoder().decode(bytes);
}

function clampTextLength(text: string): string {
  const encoded = new TextEncoder().encode(text);
  if (encoded.length > MAX_TEXT_LEN) {
    // Truncate while respecting UTF-8 boundaries
    let len = MAX_TEXT_LEN;
    while (len > 0 && (encoded[len] & 0xc0) === 0x80) {
      len--;
    }
    return new TextDecoder().decode(encoded.slice(0, len));
  }
  return text;
}

export class Codec {
  // Frame builders (commands to device)

  static buildDeviceQuery(version: number): Uint8Array {
    return new Uint8Array([CMD_DEVICE_QUERY, version]);
  }

  static buildAppStart(appName: string): Uint8Array {
    const nameBytes = new TextEncoder().encode(appName);
    const payload = new Uint8Array(1 + nameBytes.length);
    payload[0] = CMD_APP_START;
    payload.set(nameBytes, 1);
    return payload;
  }

  static buildGetContacts(since: number = 0): Uint8Array {
    const payload = new Uint8Array(5);
    payload[0] = CMD_GET_CONTACTS;
    if (since > 0) {
      writeUint32LE(payload, 1, since);
    }
    return payload;
  }

  static buildGetChannel(idx: number): Uint8Array {
    return new Uint8Array([CMD_GET_CHANNEL, idx]);
  }

  static buildSendTxtMsg(pubkeyPrefix: string, text: string, txtType: number = TXT_TYPE_PLAIN, attempt: number = 0): Uint8Array {
    const textBytes = new TextEncoder().encode(clampTextLength(text));
    const totalLen = 1 + 1 + 1 + 4 + 6 + textBytes.length;
    const payload = new Uint8Array(totalLen);
    payload[0] = CMD_SEND_TXT_MSG;
    payload[1] = txtType;
    payload[2] = attempt;
    const ts = Math.floor(Date.now() / 1000);
    writeUint32LE(payload, 3, ts);
    const pkeyBytes = hexToBytes(pubkeyPrefix.padEnd(12, '0'));
    payload.set(pkeyBytes.slice(0, 6), 7);
    payload.set(textBytes, 13);
    return payload;
  }

  static buildSendChannelTxtMsg(channelIdx: number, text: string, txtType: number = TXT_TYPE_PLAIN): Uint8Array {
    const textBytes = new TextEncoder().encode(clampTextLength(text));
    const totalLen = 1 + 1 + 1 + 4 + textBytes.length;
    const payload = new Uint8Array(totalLen);
    payload[0] = CMD_SEND_CHANNEL_TXT_MSG;
    payload[1] = txtType;
    payload[2] = channelIdx;
    const ts = Math.floor(Date.now() / 1000);
    writeUint32LE(payload, 3, ts);
    payload.set(textBytes, 7);
    return payload;
  }

  static buildSyncNextMessage(): Uint8Array {
    return new Uint8Array([CMD_SYNC_NEXT_MESSAGE]);
  }

  static buildSendLogin(pubkeyPrefix: string, password: string): Uint8Array {
    const passBytes = new TextEncoder().encode(password);
    const totalLen = 1 + 6 + passBytes.length;
    const payload = new Uint8Array(totalLen);
    payload[0] = CMD_SEND_LOGIN;
    const pkeyBytes = hexToBytes(pubkeyPrefix.padEnd(12, '0'));
    payload.set(pkeyBytes.slice(0, 6), 1);
    payload.set(passBytes, 7);
    return payload;
  }

  // Frame parsers (responses from device)
  // Returns null if frame is malformed or undersized

  static parseContactInfo(payload: Uint8Array): ContactInfo | null {
    // RESP_CODE_CONTACT: [3, pub_key[32], type(1), flags(1), out_path_len(1),
    //   out_path[64], name[32 z-padded], last_advert_ts_u32, gps_lat_i32, gps_lon_i32, lastmod_u32]
    if (payload.length < 36 + 64 + 32 + 4) return null;

    const pubKey = payload.slice(1, 1 + PUB_KEY_SIZE);
    const type = payload[33];
    const name = readString(payload, 36 + 64, 32);
    const lastAdvertTs = readUint32LE(payload, 36 + 64 + 32);
    const isRoom = type === 1; // ADV_TYPE_ROOM

    return {
      pubKeyPrefix: bytesToHex(pubKey.slice(0, 6)),
      name,
      isRoom,
      type,
      lastAdvertTs,
    };
  }

  static parseSelfInfo(payload: Uint8Array): ContactInfo | null {
    // RESP_CODE_SELF_INFO: [5, adv_type(1), tx_power(1), max_tx_power(1), pub_key[32], lat_i32, lon_i32,
    //   multi_acks(1), advert_loc_policy(1), telemetry_mode(1), manual_add(1), freq_u32, bw_u32, sf(1), cr(1), node_name...]
    if (payload.length < 48) return null;

    const pubKey = payload.slice(4, 4 + PUB_KEY_SIZE);
    const nodeNameStart = 48;
    const nodeName = readString(payload, nodeNameStart, Math.max(0, payload.length - nodeNameStart));

    return {
      pubKeyPrefix: bytesToHex(pubKey.slice(0, 6)),
      name: nodeName || 'self',
      isRoom: false,
    };
  }

  static parseChannelInfo(payload: Uint8Array): ChannelInfo | null {
    // RESP_CODE_CHANNEL_INFO: [18, channel_idx(1), name[32 z-padded], secret[16]]
    if (payload.length < 1 + 1 + 32) return null;

    const idx = payload[1];
    const name = readString(payload, 2, 32);

    return { idx, name };
  }

  static parseContactMsgRecvV3(payload: Uint8Array): TextMessage | null {
    // RESP_CODE_CONTACT_MSG_RECV_V3: [20, pub_key[32], ts_u32, text...]
    if (payload.length < 1 + 32 + 4) return null;

    const pubKey = payload.slice(1, 1 + PUB_KEY_SIZE);
    const ts = readUint32LE(payload, 1 + PUB_KEY_SIZE);
    const textBytes = payload.slice(1 + PUB_KEY_SIZE + 4);
    const text = new TextDecoder().decode(textBytes);

    return {
      pubKeyPrefix: bytesToHex(pubKey.slice(0, 6)),
      text,
      ts,
    };
  }

  static parseChannelMsgRecvV3(payload: Uint8Array): (TextMessage & { channelIdx: number }) | null {
    // RESP_CODE_CHANNEL_MSG_RECV_V3: [21, channel_idx(1), ts_u32, text...]
    if (payload.length < 1 + 1 + 4) return null;

    const channelIdx = payload[1];
    const ts = readUint32LE(payload, 2);
    const textBytes = payload.slice(6);
    const text = new TextDecoder().decode(textBytes);

    return {
      channelIdx,
      text,
      ts,
    };
  }
}
