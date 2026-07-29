import { Channel } from '../api';
import styles from './ChannelList.module.css';

interface ChannelListProps {
  channels: Channel[];
  selectedChannelId: string | null;
  onSelectChannel: (channelId: string) => void;
}

export function ChannelList({
  channels,
  selectedChannelId,
  onSelectChannel,
}: ChannelListProps) {
  // Group channels by kind
  const rooms = channels.filter((ch) => ch.kind === 'room');
  const groupChannels = channels.filter((ch) => ch.kind === 'channel');
  const dms = channels.filter((ch) => ch.kind === 'dm');

  const renderChannelItem = (channel: Channel) => {
    const isSelected = channel.id === selectedChannelId;
    const isLocked = channel.locked && channel.kind === 'room';

    return (
      <div
        key={channel.id}
        className={`${styles.item} ${isSelected ? styles.selected : ''}`}
        onClick={() => onSelectChannel(channel.id)}
      >
        <span className={styles.name}>
          {isLocked && <span className={styles.lock}>🔒</span>}
          {channel.displayName}
        </span>
        {channel.unread > 0 && (
          <span className={styles.unread}>{channel.unread}</span>
        )}
      </div>
    );
  };

  return (
    <div className={styles.sidebar}>
      <div className={styles.header}>Channels</div>

      {rooms.length > 0 && (
        <div className={styles.section}>
          <div className={styles.sectionTitle}>Rooms</div>
          {rooms.map(renderChannelItem)}
        </div>
      )}

      {groupChannels.length > 0 && (
        <div className={styles.section}>
          <div className={styles.sectionTitle}>Channels</div>
          {groupChannels.map(renderChannelItem)}
        </div>
      )}

      {dms.length > 0 && (
        <div className={styles.section}>
          <div className={styles.sectionTitle}>Direct Messages</div>
          {dms.map(renderChannelItem)}
        </div>
      )}

      {channels.length === 0 && (
        <div className={styles.empty}>No channels available</div>
      )}
    </div>
  );
}
