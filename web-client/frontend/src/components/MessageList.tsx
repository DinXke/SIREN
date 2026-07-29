import { Message } from '../api';
import styles from './MessageList.module.css';

interface MessageListProps {
  messages: Message[];
}

function formatTime(ts: number): string {
  const date = new Date(ts * 1000);
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

export function MessageList({ messages }: MessageListProps) {
  return (
    <div className={styles.list}>
      {messages.map((msg) => (
        <div
          key={msg.id}
          className={`${styles.message} ${msg.self ? styles.self : ''}`}
        >
          <div className={styles.header}>
            <span className={styles.from}>
              {msg.self ? 'You' : msg.from || '(unknown)'}
            </span>
            <span className={styles.time}>{formatTime(msg.ts)}</span>
          </div>
          <div className={styles.text}>{msg.text}</div>
          {msg.status !== 'confirmed' && msg.self && (
            <div className={styles.status}>
              {msg.status === 'pending' && '⏱ pending'}
              {msg.status === 'sent' && '✓ sent'}
              {msg.status === 'failed' && '✗ failed'}
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
