// API client for SIREN web-client

export type ConnState = 'disconnected' | 'connecting' | 'connected' | 'error';

export interface SerialPort {
  path: string;
  label: string;
}

export interface Channel {
  id: string;
  kind: 'room' | 'channel' | 'dm';
  name: string;
  displayName: string;
  locked: boolean;
  joined: boolean;
  unread: number;
}

export interface User {
  pubkeyPrefix: string;
  name: string;
  isRoom: boolean;
  lastSeen: number | null;
}

export interface Message {
  id: string;
  channelId: string;
  from: string;
  self: boolean;
  text: string;
  ts: number;
  status: 'pending' | 'sent' | 'confirmed' | 'failed';
}

export interface AppState {
  conn: ConnState;
  self: User | null;
  channels: Channel[];
  users: User[];
}

export interface WSEvent {
  type: string;
}

export interface ConnEvent extends WSEvent {
  type: 'conn';
  state: ConnState;
  self?: User;
  error?: string;
}

export interface ChannelsEvent extends WSEvent {
  type: 'channels';
  channels: Channel[];
}

export interface UsersEvent extends WSEvent {
  type: 'users';
  users: User[];
}

export interface MessageEvent extends WSEvent {
  type: 'message';
  message: Message;
}

export interface LoginEvent extends WSEvent {
  type: 'login';
  channelId: string;
  ok: boolean;
  error?: string;
}

export interface NoticeEvent extends WSEvent {
  type: 'notice';
  level: 'info' | 'warn' | 'error';
  text: string;
}

const BASE_URL = 'http://127.0.0.1:8760';

class APIClient {
  private ws: WebSocket | null = null;
  private wsReconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private listeners: Map<string, Set<(event: any) => void>> = new Map();

  async getPorts(): Promise<SerialPort[]> {
    const res = await fetch(`${BASE_URL}/api/ports`);
    if (!res.ok) throw new Error(`Failed to get ports: ${res.statusText}`);
    const data = await res.json();
    return data.ports || [];
  }

  async connect(path: string, baud?: number): Promise<{ state: ConnState; self?: User }> {
    const res = await fetch(`${BASE_URL}/api/connect`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, baud }),
    });
    if (!res.ok) {
      const error = await res.json();
      throw new Error(error.error || 'Connection failed');
    }
    return res.json();
  }

  async disconnect(): Promise<{ state: ConnState }> {
    const res = await fetch(`${BASE_URL}/api/disconnect`, { method: 'POST' });
    if (!res.ok) throw new Error('Disconnect failed');
    return res.json();
  }

  async getState(): Promise<AppState> {
    const res = await fetch(`${BASE_URL}/api/state`);
    if (!res.ok) throw new Error('Failed to get state');
    return res.json();
  }

  async getMessages(channelId: string, limit: number = 100): Promise<Message[]> {
    const res = await fetch(`${BASE_URL}/api/channels/${encodeURIComponent(channelId)}/messages?limit=${limit}`);
    if (!res.ok) throw new Error('Failed to get messages');
    const data = await res.json();
    return data.messages || [];
  }

  async joinChannel(channelId: string, password?: string): Promise<{ ok: boolean; channel: Channel; error?: string }> {
    const res = await fetch(`${BASE_URL}/api/channels/${encodeURIComponent(channelId)}/join`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ password }),
    });
    const data = await res.json();
    return data;
  }

  async partChannel(channelId: string): Promise<{ ok: boolean }> {
    const res = await fetch(`${BASE_URL}/api/channels/${encodeURIComponent(channelId)}/part`, {
      method: 'POST',
    });
    if (!res.ok) throw new Error('Failed to part channel');
    return res.json();
  }

  async sendMessage(channelId: string, text: string): Promise<{ ok: boolean; message: Message }> {
    const res = await fetch(`${BASE_URL}/api/channels/${encodeURIComponent(channelId)}/messages`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text }),
    });
    const data = await res.json();
    return data;
  }

  async setAdvert(name?: string, flood?: boolean): Promise<{ ok: boolean }> {
    const res = await fetch(`${BASE_URL}/api/advert`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, flood }),
    });
    if (!res.ok) throw new Error('Failed to set advert');
    return res.json();
  }

  connectWebSocket(): Promise<void> {
    return new Promise((resolve, reject) => {
      try {
        this.ws = new WebSocket('ws://127.0.0.1:8760/ws');

        this.ws.onopen = () => {
          console.log('WebSocket connected');
          resolve();
        };

        this.ws.onmessage = (event) => {
          try {
            const msg = JSON.parse(event.data);
            this.emit(msg.type, msg);
          } catch (e) {
            console.error('Failed to parse WS message:', e);
          }
        };

        this.ws.onerror = (event) => {
          console.error('WebSocket error:', event);
          reject(event);
        };

        this.ws.onclose = () => {
          console.log('WebSocket closed');
          this.ws = null;
          this.scheduleReconnect();
        };
      } catch (e) {
        reject(e);
      }
    });
  }

  private scheduleReconnect() {
    if (this.wsReconnectTimer) clearTimeout(this.wsReconnectTimer);
    this.wsReconnectTimer = setTimeout(() => {
      console.log('Attempting WebSocket reconnect...');
      this.connectWebSocket().catch((e) => console.error('Reconnect failed:', e));
    }, 3000);
  }

  disconnectWebSocket() {
    if (this.wsReconnectTimer) {
      clearTimeout(this.wsReconnectTimer);
      this.wsReconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  on(type: string, listener: (event: any) => void) {
    if (!this.listeners.has(type)) {
      this.listeners.set(type, new Set());
    }
    this.listeners.get(type)!.add(listener);
  }

  off(type: string, listener: (event: any) => void) {
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
}

export const api = new APIClient();
