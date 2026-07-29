import { useState, useEffect } from 'react';
import { api, SerialPort } from '../api';
import styles from './ConnectionPanel.module.css';

type TabType = 'serial' | 'bluetooth';

export function ConnectionPanel() {
  const [tab, setTab] = useState<TabType>('serial');
  const [ports, setPorts] = useState<SerialPort[]>([]);
  const [selectedPort, setSelectedPort] = useState<string>('');
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isConnected, setIsConnected] = useState(false);

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
          <span className={styles.indicator}>●</span>
          <span className={styles.status}>Connected</span>
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
              Serial
            </button>
            <button
              className={`${styles.tab} ${tab === 'bluetooth' ? styles.active : ''}`}
              onClick={() => setTab('bluetooth')}
              disabled={isLoading || !bleAvailable}
              title={!bleAvailable ? 'Bluetooth not available' : ''}
            >
              Bluetooth
            </button>
          </div>

          {tab === 'serial' && (
            <div className={styles.content}>
              <select
                value={selectedPort}
                onChange={(e) => setSelectedPort(e.target.value)}
                disabled={isLoading || ports.length === 0}
                className={styles.select}
              >
                <option value="">Select a serial port...</option>
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
          )}

          {tab === 'bluetooth' && (
            <div className={styles.content}>
              {!bleAvailable ? (
                <div className={styles.bleUnavailable}>
                  <p>
                    Bluetooth requires a secure context — open via{' '}
                    <code>http://localhost:8760</code> or use HTTPS for remote access.
                  </p>
                </div>
              ) : (
                <button
                  className={styles.button}
                  onClick={handleBleConnect}
                  disabled={isLoading}
                >
                  {isLoading ? 'Scanning...' : 'Scan & Connect'}
                </button>
              )}
            </div>
          )}

          {error && <div className={styles.error}>{error}</div>}
        </div>
      )}
    </div>
  );
}
