import { useState, useEffect } from 'react';
import { api, SerialPort, ConnState } from '../api';
import styles from './ConnectionPanel.module.css';

interface ConnectionPanelProps {
  connState?: ConnState;
}

export function ConnectionPanel({ connState = 'disconnected' }: ConnectionPanelProps) {
  const [ports, setPorts] = useState<SerialPort[]>([]);
  const [selectedPort, setSelectedPort] = useState<string>('');
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isConnected, setIsConnected] = useState(false);

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

  const handleConnect = async () => {
    if (!selectedPort) {
      setError('Please select a serial port');
      return;
    }

    setIsLoading(true);
    setError(null);

    try {
      const result = await api.connect(selectedPort);
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
            onClick={handleConnect}
            disabled={isLoading || !selectedPort}
          >
            {isLoading ? 'Connecting...' : 'Connect'}
          </button>
          {error && <div className={styles.error}>{error}</div>}
        </div>
      )}
    </div>
  );
}
