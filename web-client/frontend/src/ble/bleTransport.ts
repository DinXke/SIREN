// BLE Transport implementation for SIREN
// Pure browser Web Bluetooth transport with retry logic

import { Transport } from '../transport/types';
import { Message, ConnState } from '../api';
import { Codec } from './codec';
import { SirenState } from './sirenState';
import { NUS_SERVICE_UUID, NUS_RX_UUID, NUS_TX_UUID, RESP_CODE_DEVICE_INFO, RESP_CODE_SELF_INFO, RESP_CODE_CONTACTS_START, RESP_CODE_CONTACT, RESP_CODE_END_OF_CONTACTS, RESP_CODE_CHANNEL_INFO, RESP_CODE_CONTACT_MSG_RECV_V3, RESP_CODE_CHANNEL_MSG_RECV_V3, RESP_CODE_ERR } from './types';

export class BleTransport implements Transport {
  private device: BluetoothDevice | null = null;
  private rxChar: BluetoothRemoteGATTCharacteristic | null = null;
  private txChar: BluetoothRemoteGATTCharacteristic | null = null;
  private state: SirenState = new SirenState();
  private listeners: Map<string, Set<(event: any) => void>> = new Map();
  private connState: ConnState = 'disconnected';
  private nextChannelIdx: number = 0;
  private syncComplete: Promise<void> | null = null;
  private syncResolver: (() => void) | null = null;
  private onDisconnectHandler: (() => void) | null = null;
  private onNotificationHandler: ((e: Event) => void) | null = null;

  async connect(): Promise<void> {
    try {
      // Reset state for new connection
      this.state = new SirenState();
      this.nextChannelIdx = 0;

      // Clean up any previous device
      if (this.device) {
        this.cleanupDevice();
      }

      this.device = await navigator.bluetooth!.requestDevice({
        filters: [{ services: [NUS_SERVICE_UUID] }],
      });

      // SC/MITM bonding causes transient GATT disconnect right after pairing.
      // Retry with exponential backoff.
      let lastErr: any;
      const maxAttempts = 5;

      for (let attempt = 0; attempt < maxAttempts; attempt++) {
        try {
          if (attempt > 0) {
            const delayMs = Math.min(500 * Math.pow(2, attempt - 1), 10000);
            await new Promise((r) => setTimeout(r, delayMs));
          }

          const server = await this.device!.gatt!.connect();
          const service = await server.getPrimaryService(NUS_SERVICE_UUID);
          this.rxChar = await service.getCharacteristic(NUS_RX_UUID);
          this.txChar = await service.getCharacteristic(NUS_TX_UUID);

          // Register listener BEFORE starting notifications to avoid race condition
          // where notifications arrive before the listener is registered
          this.onNotificationHandler = (e: Event) => {
            const target = e.target as BluetoothRemoteGATTCharacteristic;
            const payload = new Uint8Array(target.value!.buffer);
            this.handleFrame(payload);
          };
          this.txChar.addEventListener('characteristicvaluechanged', this.onNotificationHandler);

          await this.txChar.startNotifications();

          // Register disconnect handler only AFTER full setup.
          // Remove old handler first to prevent listener accumulation across retries.
          if (this.onDisconnectHandler) {
            this.device!.removeEventListener('gattserverdisconnected', this.onDisconnectHandler);
          }
          this.onDisconnectHandler = () => {
            this.cleanupDevice();
            this.connState = 'disconnected';
            this.emit('conn', { type: 'conn', state: 'disconnected' });
          };
          this.device.addEventListener('gattserverdisconnected', this.onDisconnectHandler);

          // Connected successfully
          this.connState = 'connected';
          this.emit('conn', { type: 'conn', state: 'connected', self: this.state.self });

          try {
            await this.handshake();
          } catch (handshakeErr) {
            // Handshake failed, clean up handlers before throwing
            this.cleanupDevice();
            throw handshakeErr;
          }
          return;
        } catch (e) {
          lastErr = e;
          const errMsg = ((e as Error).message || '').toLowerCase();
          const isGattErr = errMsg.includes('gatt') || errMsg.includes('disconnected') || (e as any).name === 'NetworkError';

          if (!isGattErr || attempt === maxAttempts - 1) {
            // On final attempt or non-GATT error, clean up before throwing
            this.cleanupDevice();
            throw e;
          }
          // On retry, cleanupDevice will be called at start of next iteration
        }
      }

      throw lastErr;
    } catch (err) {
      const errMsg = ((err as Error).message || '').toLowerCase();
      const isGattDisconnect = errMsg.includes('gatt') && errMsg.includes('disconnected');

      if (isGattDisconnect) {
        throw new Error(
          `Bluetooth-koppeling vereist. Zorg dat de radio is gekoppeld in Windows-instellingen > Bluetooth (PIN 123456). Sluit andere Bluetooth-apps, start de browser opnieuw, en probeer het opnieuw. Of gebruik de Serial-verbinding via USB. [${(err as any).name}]`
        );
      }

      this.connState = 'error';
      this.emit('conn', { type: 'conn', state: 'error', error: (err as Error).message });
      throw new Error(`BLE connect failed: ${(err as Error).message}`);
    }
  }

  async disconnect(): Promise<void> {
    if (this.device && this.device.gatt && this.device.gatt.connected) {
      await this.device.gatt.disconnect();
    }
    this.cleanupDevice();
    this.connState = 'disconnected';
    this.emit('conn', { type: 'conn', state: 'disconnected' });
  }

  private cleanupDevice(): void {
    // Remove event listeners before cleaning up
    if (this.device && this.onDisconnectHandler) {
      this.device.removeEventListener('gattserverdisconnected', this.onDisconnectHandler);
      this.onDisconnectHandler = null;
    }
    if (this.txChar && this.onNotificationHandler) {
      this.txChar.removeEventListener('characteristicvaluechanged', this.onNotificationHandler);
      this.onNotificationHandler = null;
    }
    this.device = null;
    this.rxChar = null;
    this.txChar = null;
  }

  async send(payload: Uint8Array): Promise<void> {
    if (!this.rxChar) throw new Error('BLE not connected');
    await this.rxChar.writeValue(payload as any);
    // Throttle writes: ~60ms apart
    await new Promise((r) => setTimeout(r, 60));
  }

  async getState(): Promise<any> {
    return {
      conn: this.connState,
      self: this.state.self,
      channels: this.state.getChannels(),
      users: this.state.getUsers(),
    };
  }

  async getMessages(channelId: string, limit: number = 100): Promise<Message[]> {
    return this.state.getMessages(channelId).slice(-limit);
  }

  async joinChannel(channelId: string, password?: string): Promise<any> {
    if (channelId.startsWith('room:')) {
      const pubKeyPrefix = channelId.replace('room:', '');
      await this.send(Codec.buildSendLogin(pubKeyPrefix, password ?? ''));
      return { ok: true };
    }
    return { ok: false, error: 'Unsupported channel type' };
  }

  async partChannel(_channelId: string): Promise<any> {
    // BLE doesn't have an explicit part command; just don't listen
    return { ok: true };
  }

  async sendMessage(channelId: string, text: string): Promise<any> {
    let msgId = '';
    try {
      if (channelId.startsWith('room:')) {
        const pubKeyPrefix = channelId.replace('room:', '');
        await this.send(Codec.buildSendTxtMsg(pubKeyPrefix, text));
        msgId = `msg-${this.state.nextMessageId++}`;
        const now = Math.floor(Date.now() / 1000);
        const msg: Message = {
          id: msgId,
          channelId,
          from: this.state.self?.name || 'unknown',
          self: true,
          text,
          ts: now,
          status: 'pending',
        };
        this.state.addMessage(msg);
        return { ok: true, message: msg };
      } else if (channelId.startsWith('chan:')) {
        const channelIdx = parseInt(channelId.replace('chan:', ''), 10);
        await this.send(Codec.buildSendChannelTxtMsg(channelIdx, text));
        msgId = `msg-${this.state.nextMessageId++}`;
        const now = Math.floor(Date.now() / 1000);
        const msg: Message = {
          id: msgId,
          channelId,
          from: this.state.self?.name || 'unknown',
          self: true,
          text,
          ts: now,
          status: 'pending',
        };
        this.state.addMessage(msg);
        return { ok: true, message: msg };
      }
      return { ok: false, error: 'Unsupported channel type' };
    } catch (e) {
      if (msgId) {
        this.state.updateMessage(msgId, { status: 'failed' });
      }
      throw e;
    }
  }

  async setAdvert(_name?: string, _flood?: boolean): Promise<any> {
    // BLE-only transport doesn't have advert control from this side
    return { ok: true };
  }

  on(type: string, listener: (event: any) => void): void {
    if (!this.listeners.has(type)) {
      this.listeners.set(type, new Set());
    }
    this.listeners.get(type)!.add(listener);
  }

  off(type: string, listener: (event: any) => void): void {
    const set = this.listeners.get(type);
    if (set) {
      set.delete(listener);
    }
  }

  private emit(type: string, event: any) {
    const set = this.listeners.get(type);
    if (set) {
      set.forEach((listener) => listener(event));
    }
  }

  private async handshake(): Promise<void> {
    try {
      // Create a promise that resolves when sync completes
      this.syncComplete = new Promise((resolve) => {
        this.syncResolver = resolve;
      });

      // 1. CMD_DEVICE_QUERY
      await this.send(Codec.buildDeviceQuery(3));
      // 2. CMD_APP_START
      await new Promise((r) => setTimeout(r, 100));
      await this.send(Codec.buildAppStart('siren-react'));
      // 3. CMD_GET_CONTACTS
      await new Promise((r) => setTimeout(r, 100));
      await this.send(Codec.buildGetContacts());

      // Wait for sync to complete (END_OF_CONTACTS + all channels)
      // The async handlers will populate contacts/channels and signal completion via ERR
      // Timeout after 5 seconds to avoid hanging
      const timeoutPromise = new Promise<void>((_, reject) =>
        setTimeout(() => reject(new Error('BLE handshake timeout: no response from device')), 5000)
      );

      await Promise.race([this.syncComplete, timeoutPromise]);
    } catch (e) {
      console.error('BLE handshake error:', e);
      throw e;
    }
  }

  private async drainMessages(): Promise<void> {
    if (this.connState !== 'connected') return;
    try {
      await this.send(Codec.buildSyncNextMessage());
    } catch (e) {
      console.error('BLE drain error:', e);
    }
  }

  private handleFrame(payload: Uint8Array) {
    if (!payload || payload.length < 1) return;

    const code = payload[0];

    try {
      if (code === RESP_CODE_DEVICE_INFO) {
        // Informational, safe to ignore
      } else if (code === RESP_CODE_SELF_INFO) {
        const info = Codec.parseSelfInfo(payload);
        if (info) {
          this.state.setSelf(info);
          this.emit('conn', { type: 'conn', state: 'connected', self: info });
        }
      } else if (code === RESP_CODE_CONTACTS_START) {
        // Expect CONTACT frames to follow
      } else if (code === RESP_CODE_CONTACT) {
        const contact = Codec.parseContactInfo(payload);
        if (contact) {
          this.state.addContact(contact);
          this.emit('users', { type: 'users', users: this.state.getUsers() });
          this.emit('channels', { type: 'channels', channels: this.state.getChannels() });
        }
      } else if (code === RESP_CODE_END_OF_CONTACTS) {
        // Contacts finished, now get channels
        this.getNextChannel();
      } else if (code === RESP_CODE_CHANNEL_INFO) {
        const ch = Codec.parseChannelInfo(payload);
        if (ch) {
          this.state.addChannel(ch);
          this.emit('channels', { type: 'channels', channels: this.state.getChannels() });
          // Continue fetching channels
          setTimeout(() => this.getNextChannel(), 50);
        }
      } else if (code === RESP_CODE_ERR) {
        // No more channels, sync complete
        if (this.syncResolver) {
          this.syncResolver();
          this.syncResolver = null;
        }
        // Start draining messages
        this.drainMessages();
      } else if (code === RESP_CODE_CONTACT_MSG_RECV_V3) {
        const msg = Codec.parseContactMsgRecvV3(payload);
        if (msg && msg.pubKeyPrefix) {
          const msgId = `msg-${this.state.nextMessageId++}`;
          const message: Message = {
            id: msgId,
            channelId: `room:${msg.pubKeyPrefix}`,
            from: this.findContactName(msg.pubKeyPrefix),
            self: false,
            text: msg.text,
            ts: msg.ts,
            status: 'confirmed',
          };
          this.state.addMessage(message);
          this.emit('message', { type: 'message', message });
        }
        // Drain next message
        this.drainMessages();
      } else if (code === RESP_CODE_CHANNEL_MSG_RECV_V3) {
        const msg = Codec.parseChannelMsgRecvV3(payload);
        if (msg) {
          const msgId = `msg-${this.state.nextMessageId++}`;
          const message: Message = {
            id: msgId,
            channelId: `chan:${msg.channelIdx}`,
            from: 'room',
            self: false,
            text: msg.text,
            ts: msg.ts,
            status: 'confirmed',
          };
          this.state.addMessage(message);
          this.emit('message', { type: 'message', message });
        }
        // Drain next message
        this.drainMessages();
      }
    } catch (e) {
      console.error('Error handling BLE frame:', e);
    }
  }

  private findContactName(pubKeyPrefix: string): string {
    const contact = this.state.contacts.get(pubKeyPrefix);
    return contact ? contact.name : pubKeyPrefix;
  }

  private getNextChannel(): void {
    if (this.connState === 'connected') {
      this.send(Codec.buildGetChannel(this.nextChannelIdx++)).catch((e) => console.error('Get channel error:', e));
    }
  }
}
