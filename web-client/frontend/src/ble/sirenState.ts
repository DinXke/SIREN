// BLE state management for SIREN
// Tracks contacts, channels, messages and maps to app's data types

import { Channel, User, Message } from '../api';
import { ContactInfo, ChannelInfo } from './types';

export class SirenState {
  self: ContactInfo | null = null;
  contacts: Map<string, ContactInfo> = new Map();
  channels: Map<number, ChannelInfo> = new Map();
  messages: Map<string, Message> = new Map();
  nextMessageId: number = 1;
  pendingAcks: Map<number, { messageId: string; ts: number }> = new Map();

  onStateChange: (() => void) | null = null;
  onMessage: ((msg: Message) => void) | null = null;

  setSelf(contact: ContactInfo) {
    this.self = contact;
    if (this.onStateChange) this.onStateChange();
  }

  addContact(contact: ContactInfo) {
    this.contacts.set(contact.pubKeyPrefix, contact);
    if (this.onStateChange) this.onStateChange();
  }

  addChannel(channel: ChannelInfo) {
    this.channels.set(channel.idx, channel);
    if (this.onStateChange) this.onStateChange();
  }

  addMessage(msg: Message) {
    this.messages.set(msg.id, msg);
    if (this.onMessage) this.onMessage(msg);
  }

  updateMessage(id: string, updates: Partial<Message>) {
    const msg = this.messages.get(id);
    if (msg) {
      Object.assign(msg, updates);
      if (this.onMessage) this.onMessage(msg);
    }
  }

  getChannels(): Channel[] {
    const result: Channel[] = [];

    // Rooms
    for (const c of this.contacts.values()) {
      if (c.isRoom) {
        result.push({
          id: `room:${c.pubKeyPrefix}`,
          kind: 'room',
          name: c.name,
          displayName: `#siren-${c.name}`,
          locked: false,
          joined: false,
          unread: 0,
        });
      }
    }

    // Channels
    for (const ch of this.channels.values()) {
      result.push({
        id: `chan:${ch.idx}`,
        kind: 'channel',
        name: ch.name,
        displayName: `#${ch.name}`,
        locked: false,
        joined: false,
        unread: 0,
      });
    }

    // DMs
    for (const c of this.contacts.values()) {
      if (!c.isRoom) {
        result.push({
          id: `dm:${c.pubKeyPrefix}`,
          kind: 'dm',
          name: c.name,
          displayName: c.name,
          locked: false,
          joined: false,
          unread: 0,
        });
      }
    }

    return result;
  }

  getUsers(): User[] {
    const result: User[] = [];
    for (const c of this.contacts.values()) {
      result.push({
        pubkeyPrefix: c.pubKeyPrefix,
        name: c.name,
        isRoom: c.isRoom,
        lastSeen: c.lastAdvertTs || null,
      });
    }
    return result;
  }

  getMessages(channelId: string): Message[] {
    return Array.from(this.messages.values()).filter((m) => m.channelId === channelId);
  }
}
