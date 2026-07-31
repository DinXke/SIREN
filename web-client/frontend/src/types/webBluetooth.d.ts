// Web Bluetooth API type definitions
declare global {
  interface Navigator {
    bluetooth?: Bluetooth;
  }

  interface Bluetooth {
    requestDevice(options: RequestDeviceOptions): Promise<BluetoothDevice>;
    getAvailability(): Promise<boolean>;
    getDevices(): Promise<BluetoothDevice[]>;
  }

  interface RequestDeviceOptions {
    filters?: BluetoothLEScanFilterInit[];
    optionalServices?: string[];
  }

  interface BluetoothLEScanFilterInit {
    services?: string[];
    name?: string;
    namePrefix?: string;
  }

  interface BluetoothDevice extends EventTarget {
    id: string;
    name?: string;
    gatt?: BluetoothRemoteGATTServer;
    addEventListener(
      type: 'gattserverdisconnected',
      listener: EventListener,
      options?: boolean | AddEventListenerOptions
    ): void;
    removeEventListener(
      type: 'gattserverdisconnected',
      listener: EventListener,
      options?: boolean | EventListenerOptions
    ): void;
  }

  interface BluetoothRemoteGATTServer {
    device: BluetoothDevice;
    connected: boolean;
    connect(): Promise<BluetoothRemoteGATTServer>;
    disconnect(): void;
    getPrimaryService(
      service: string | number
    ): Promise<BluetoothRemoteGATTService>;
    getPrimaryServices(
      service?: string | number
    ): Promise<BluetoothRemoteGATTService[]>;
  }

  interface BluetoothRemoteGATTService extends EventTarget {
    device: BluetoothDevice;
    uuid: string;
    isPrimary: boolean;
    getCharacteristic(
      characteristic: string | number
    ): Promise<BluetoothRemoteGATTCharacteristic>;
    getCharacteristics(
      characteristic?: string | number
    ): Promise<BluetoothRemoteGATTCharacteristic[]>;
  }

  interface BluetoothRemoteGATTCharacteristic extends EventTarget {
    service: BluetoothRemoteGATTService;
    uuid: string;
    properties: BluetoothCharacteristicProperties;
    value?: DataView;
    readValue(): Promise<DataView>;
    writeValue(value: BufferSource): Promise<void>;
    writeValueWithResponse(value: BufferSource): Promise<void>;
    writeValueWithoutResponse(value: BufferSource): Promise<void>;
    startNotifications(): Promise<BluetoothRemoteGATTCharacteristic>;
    stopNotifications(): Promise<BluetoothRemoteGATTCharacteristic>;
    addEventListener(
      type: 'characteristicvaluechanged',
      listener: EventListener,
      options?: boolean | AddEventListenerOptions
    ): void;
    removeEventListener(
      type: 'characteristicvaluechanged',
      listener: EventListener,
      options?: boolean | EventListenerOptions
    ): void;
  }

  interface BluetoothCharacteristicProperties {
    broadcast: boolean;
    read: boolean;
    writeWithoutResponse: boolean;
    write: boolean;
    notify: boolean;
    indicate: boolean;
    authenticatedSignedWrites: boolean;
    reliableWrite: boolean;
    writableAuxiliaries: boolean;
  }
}

export {};
