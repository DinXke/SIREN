import { useState, useEffect } from 'react';
import { api, SerialPort } from '../api';
import styles from './ConnectionPanel.module.css';

type TabType = 'serial' | 'bluetooth';

// Device type icons
function getDeviceIcon(deviceType: string) {
  switch (deviceType?.toLowerCase()) {
    case 'room':
    case 'room server':
      return '🏢';
    case 'repeater':
      return '📡';
    case 'companion':
      return '📱';
    default:
      return '🔗';
  }
}

function getDeviceTypeLabel(deviceType: string | undefined) {
  if (!deviceType) return 'Device';
  return deviceType.charAt(0).toUpperCase() + deviceType.slice(1);
}

export function ConnectionPanel() {
  const [tab, setTab] = useState<TabType>('serial');
  const [ports, setPorts] = useState<SerialPort[]>([]);
  const [selectedPort, setSelectedPort] = useState<string>('');
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isConnected, setIsConnected] = useState(false);
  const [connectedDeviceType, setConnectedDeviceType] = useState<string | null>(null);

  // Feature detection
  const hasBluetoothSupport = !!navigator.bluetooth;
  const isSecureContext = window.isSecureContext;
  const bleAvailable = hasBluetoothSupport && isSecureContext;

  // Fetch available ports
  useEffect(() => {
    const loadPorts = async () => {
      try {
        const availablePorts = await api.getPorts();
        setPorts(availablePorts);
        if (availablePorts.length > 0 && !selectedPort) {
          setSelectedPort(availablePorts[0].path);
        }
      } catch (e) {
        setError(e instanceof Error ? e.message : 'Failed to load ports');
      }
    };

    loadPorts();
  }, [selectedPort]);

  // Listen for connection state changes
  useEffect(() => {
    const handleConnEvent = (event: any) => {
      setIsConnected(event.state === 'connected');
      if (event.state === 'connected') {
        setError(null);
        // Try to detect device type from state
        setConnectedDeviceType('companion');
      } else if (event.state === 'error') {
        setError(event.error || 'Connection error');
      }
    };

    api.on('conn', handleConnEvent);
    return () => api.off('conn', handleConnEvent);
  }, []);

  const handleSerialConnect = async () => {
    if (!selectedPort) {
      setError('Please select a serial port');
      return;
    }

    setIsLoading(true);
    setError(null);

    try {
      const result = await api.connectSerial(selectedPort);
      setIsConnected(result.state === 'connected');
      if (result.state !== 'connected') {
        setError('Connection failed');
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Connection failed');
    } finally {
      setIsLoading(false);
    }
  };

  const handleBleConnect = async () => {
    setIsLoading(true);
    setError(null);

    try {
      const result = await api.connectBle();
      setIsConnected(result.state === 'connected');
      if (result.state !== 'connected') {
        setError('Connection failed');
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Connection failed');
    } finally {
      setIsLoading(false);
    }
  };

  const handleDisconnect = async () => {
    setIsLoading(true);
    try {
      await api.disconnect();
      setIsConnected(false);
      setConnectedDeviceType(null);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Disconnect failed');
    } finally {
      setIsLoading(false);
    }
  };

  return (
    <div className={styles.panel}>
      {isConnected ? (
        <div className={styles.connected}>
          <div className={styles.indicator} />
          <div>
            <div className={styles.status}>Connected</div>
            {connectedDeviceType && (
              <div className={styles.deviceInfo}>
                <span className={styles.deviceIcon}>
                  {getDeviceIcon(connectedDeviceType)}
                </span>
                <span>{getDeviceTypeLabel(connectedDeviceType)}</span>
              </div>
            )}
          </div>
          <button
            className={styles.button}
            onClick={handleDisconnect}
            disabled={isLoading}
          >
            Disconnect
          </button>
        </div>
      ) : (
        <div className={styles.disconnected}>
          <div className={styles.tabs}>
            <button
              className={`${styles.tab} ${tab === 'serial' ? styles.active : ''}`}
              onClick={() => setTab('serial')}
              disabled={isLoading}
            >
              <span className={styles.tabIcon}>🔌</span>
              <span>Serial</span>
            </button>
            <button
              className={`${styles.tab} ${tab === 'bluetooth' ? styles.active : ''}`}
              onClick={() => setTab('bluetooth')}
              disabled={isLoading || !bleAvailable}
              title={!bleAvailable ? 'Bluetooth not available' : ''}
            >
              <span className={styles.tabIcon}>📡</span>
              <span>Wireless</span>
            </button>
          </div>

          {tab === 'serial' && (
            <div className={styles.content}>
              <div className={styles.contentRow}>
                <select
                  value={selectedPort}
                  onChange={(e) => setSelectedPort(e.target.value)}
                  disabled={isLoading || ports.length === 0}
                  className={styles.select}
                >
                  <option value="">Select a device...</option>
                  {ports.map((port) => (
                    <option key={port.path} value={port.path}>
                      {port.label || port.path}
                    </option>
                  ))}
                </select>
                <button
                  className={styles.button}
                  onClick={handleSerialConnect}
                  disabled={isLoading || !selectedPort}
                >
                  {isLoading ? 'Connecting...' : 'Connect'}
                </button>
              </div>
            </div>
          )}

          {tab === 'bluetooth' && (
            <div className={styles.content}>
              {!bleAvailable ? (
                <div className={styles.bleUnavailable}>
                  <div className={styles.bleUnavailableIcon}>⚠️</div>
                  <div>
                    <p>
                      Bluetooth requires a secure context. Use one of these options:
                    </p>
                    <ul style={{ margin: '8px 0', paddingLeft: '20px' }}>
                      <li><code>http://localhost:8760</code> (local, no HTTPS needed)</li>
                      <li><code>http://127.0.0.1:8760</code> (local, no HTTPS needed)</li>
                      <li>HTTPS for remote access</li>
                    </ul>
                  </div>
                </div>
              ) : (
                <div className={styles.contentRow}>
                  <button
                    className={styles.button}
                    onClick={handleBleConnect}
                    disabled={isLoading}
                    style={{ flex: 1 }}
                  >
                    {isLoading ? 'Scanning for devices...' : 'Scan & Connect'}
                  </button>
                </div>
              )}
            </div>
          )}

          {error && (
            <div className={styles.error}>
              <span className={styles.errorIcon}>✕</span>
              <span>{error}</span>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
