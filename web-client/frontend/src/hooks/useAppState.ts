import { useState, useEffect, useCallback } from 'react';
import { api, AppState, Channel, User, ConnState } from '../api';

export function useAppState() {
  const [state, setState] = useState<AppState>({
    conn: 'disconnected',
    self: null,
    channels: [],
    users: [],
  });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const loadState = useCallback(async () => {
    try {
      const newState = await api.getState();
      setState(newState);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to load state');
    }
  }, []);

  useEffect(() => {
    let mounted = true;

    const init = async () => {
      try {
        // Load initial state
        await loadState();

        // Connect WebSocket
        try {
          await api.connectWebSocket();
        } catch (e) {
          console.error('WebSocket connection failed:', e);
        }

        // Set up listeners for WebSocket events
        const handleConn = (event: any) => {
          if (mounted) {
            setState((prev) => ({
              ...prev,
              conn: event.state,
              self: event.self || prev.self,
            }));
          }
        };

        const handleChannels = (event: any) => {
          if (mounted) {
            setState((prev) => ({
              ...prev,
              channels: event.channels,
            }));
          }
        };

        const handleUsers = (event: any) => {
          if (mounted) {
            setState((prev) => ({
              ...prev,
              users: event.users,
            }));
          }
        };

        api.on('conn', handleConn);
        api.on('channels', handleChannels);
        api.on('users', handleUsers);

        setLoading(false);

        return () => {
          api.off('conn', handleConn);
          api.off('channels', handleChannels);
          api.off('users', handleUsers);
        };
      } catch (e) {
        if (mounted) {
          setError(e instanceof Error ? e.message : 'Failed to initialize');
          setLoading(false);
        }
      }
    };

    const cleanup = init();

    return () => {
      mounted = false;
      cleanup?.then((fn) => fn?.());
    };
  }, [loadState]);

  return { state, loading, error };
}
