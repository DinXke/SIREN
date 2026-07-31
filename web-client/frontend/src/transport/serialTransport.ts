// Serial Transport implementation for SIREN
// Uses REST API + WebSocket for device communication

import { Transport } from './types';
import { AppState, Channel, Message } from '../api';

// Derive base URL from current page so HTTP (localhost) and HTTPS (remote) both work
const BASE_URL = `${window.location.protocol}//${window.location.host}`;

export class SerialTransport implements Transport {
  private ws: WebSocket | null = null;
  private wsReconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private listeners: Map<string, Set<(event: any) => void>> = new Map();

  async connect(): Promise<void> {
    // Handled by connectWebSocket in the actual implementation
    // This method is called when activating the transport
  }

  async disconnect(): Promise<void> {
    this.disconnectWebSocket();
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
        const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        this.ws = new WebSocket(`${wsProtocol}//${window.location.host}/ws`);

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

  private disconnectWebSocket() {
    if (this.wsReconnectTimer) {
      clearTimeout(this.wsReconnectTimer);
      this.wsReconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
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
}
