import { useState, useEffect, useCallback } from 'react';
import { useAppState } from './hooks/useAppState';
import { ConnectionPanel } from './components/ConnectionPanel';
import { ChannelList } from './components/ChannelList';
import { ChatPane } from './components/ChatPane';
import { UserList } from './components/UserList';
import { api } from './api';
import styles from './App.module.css';

export function App() {
  const { state, loading, error } = useAppState();
  const [selectedChannelId, setSelectedChannelId] = useState<string | null>(null);
  const [unreadCounts, setUnreadCounts] = useState<Record<string, number>>({});

  // Listen for incoming messages to track unread counts
  useEffect(() => {
    const handleMessage = (event: { message: { channelId: string } }) => {
      const { channelId } = event.message;
      if (channelId !== selectedChannelId) {
        setUnreadCounts((prev) => ({
          ...prev,
          [channelId]: (prev[channelId] ?? 0) + 1,
        }));
      }
    };
    api.on('message', handleMessage);
    return () => api.off('message', handleMessage);
  }, [selectedChannelId]);

  const handleSelectChannel = useCallback((channelId: string) => {
    setSelectedChannelId(channelId);
    setUnreadCounts((prev) => {
      if (!prev[channelId]) return prev;
      const next = { ...prev };
      delete next[channelId];
      return next;
    });
  }, []);

  // Merge client-side unread counts with server state
  const channelsWithUnread = state.channels.map((ch) => ({
    ...ch,
    unread: (unreadCounts[ch.id] ?? 0) + ch.unread,
  }));

  const selectedChannel = selectedChannelId
    ? channelsWithUnread.find((ch) => ch.id === selectedChannelId)
    : null;

  if (loading) {
    return (
      <div className={styles.container}>
        <div className={styles.loader}>Loading SIREN IRC Client...</div>
      </div>
    );
  }

  if (error && state.conn === 'disconnected') {
    return (
      <div className={styles.container}>
        <div className={styles.errorPanel}>
          <h1>SIREN IRC Client</h1>
          <p className={styles.errorText}>{error}</p>
          <ConnectionPanel />
        </div>
      </div>
    );
  }

  return (
    <div className={styles.container}>
      <div className={styles.header}>
        <div className={styles.title}>SIREN IRC Client</div>
        <ConnectionPanel />
      </div>

      <div className={styles.main}>
        <ChannelList
          channels={channelsWithUnread}
          selectedChannelId={selectedChannelId}
          onSelectChannel={handleSelectChannel}
        />

        <div className={styles.chatArea}>
          {selectedChannel ? (
            <ChatPane
              channel={selectedChannel}
              conn={state.conn}
            />
          ) : (
            <div className={styles.noChannelSelected}>
              <p>Select a channel to start chatting</p>
            </div>
          )}
        </div>

        <UserList users={state.users} />
      </div>
    </div>
  );
}
