// BLE protocol types and constants
// Ported from web-client/siren-standalone.html

// BLE NUS (Nordic UART Service) UUIDs
export const NUS_SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
export const NUS_RX_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
export const NUS_TX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

// Command codes (must match firmware)
export const CMD_APP_START = 1;
export const CMD_DEVICE_QUERY = 22;
export const CMD_GET_CONTACTS = 4;
export const CMD_GET_CHANNEL = 31;
export const CMD_SEND_TXT_MSG = 2;
export const CMD_SEND_CHANNEL_TXT_MSG = 3;
export const CMD_SYNC_NEXT_MESSAGE = 10;
export const CMD_SEND_LOGIN = 26;

// Response codes (must match firmware)
export const RESP_CODE_OK = 0;
export const RESP_CODE_ERR = 1;
export const RESP_CODE_CONTACTS_START = 2;
export const RESP_CODE_CONTACT = 3;
export const RESP_CODE_END_OF_CONTACTS = 4;
export const RESP_CODE_SELF_INFO = 5;
export const RESP_CODE_SENT = 6;
export const RESP_CODE_DEVICE_INFO = 13;
export const RESP_CODE_CONTACT_MSG_RECV_V3 = 16;
export const RESP_CODE_CHANNEL_MSG_RECV_V3 = 17;
export const RESP_CODE_CHANNEL_INFO = 18;
export const RESP_CODE_NO_MORE_MESSAGES = 10;

// Contact types
export const ADV_TYPE_ROOM = 1;
export const ADV_TYPE_DEVICE = 2;

// Text message types
export const TXT_TYPE_PLAIN = 0;

// Constants
export const PUB_KEY_SIZE = 32;
export const MAX_TEXT_LEN = 200;

export interface ContactInfo {
  pubKeyPrefix: string;
  name: string;
  isRoom: boolean;
  type?: number;
  lastAdvertTs?: number;
}

export interface ChannelInfo {
  idx: number;
  name: string;
}

export interface TextMessage {
  pubKeyPrefix?: string;
  channelIdx?: number;
  text: string;
  ts: number;
}

export interface SelfInfo extends ContactInfo {
  isRoom: false;
}
