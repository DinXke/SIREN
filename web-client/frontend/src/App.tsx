import { useState } from 'react';
import { useAppState } from './hooks/useAppState';
import { ConnectionPanel } from './components/ConnectionPanel';
import { ChannelList } from './components/ChannelList';
import { ChatPane } from './components/ChatPane';
import { UserList } from './components/UserList';
import styles from './App.module.css';

export function App() {
  const { state, loading, error } = useAppState();
  const [selectedChannelId, setSelectedChannelId] = useState<string | null>(null);

  const selectedChannel = selectedChannelId
    ? state.channels.find((ch) => ch.id === selectedChannelId)
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
          channels={state.channels}
          selectedChannelId={selectedChannelId}
          onSelectChannel={setSelectedChannelId}
        />

        <div className={styles.chatArea}>
          {selectedChannel ? (
            <ChatPane
              channel={selectedChannel}
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
