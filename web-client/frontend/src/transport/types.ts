// Transport abstraction for SIREN web-client
// Allows pluggable connection transports (Serial, BLE, etc.)

export interface Transport {
  // Connection lifecycle
  connect(): Promise<void>;
  disconnect(): Promise<void>;

  // API methods
  getState(): Promise<any>;
  getMessages(channelId: string, limit: number): Promise<any[]>;
  joinChannel(channelId: string, password?: string): Promise<any>;
  partChannel(channelId: string): Promise<any>;
  sendMessage(channelId: string, text: string): Promise<any>;
  setAdvert(name?: string, flood?: boolean): Promise<any>;

  // Event bus (same events as current api.on/off)
  on(type: string, listener: (event: any) => void): void;
  off(type: string, listener: (event: any) => void): void;
}
