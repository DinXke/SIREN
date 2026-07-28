import { User } from '../api';
import styles from './UserList.module.css';

interface UserListProps {
  users: User[];
}

function formatLastSeen(ts: number | null): string {
  if (!ts) return 'unknown';
  const now = Math.floor(Date.now() / 1000);
  const diff = now - ts;

  if (diff < 60) return 'now';
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
  return `${Math.floor(diff / 86400)}d ago`;
}

export function UserList({ users }: UserListProps) {
  const rooms = users.filter((u) => u.isRoom);
  const contacts = users.filter((u) => !u.isRoom);

  const renderUser = (user: User) => (
    <div key={user.pubkeyPrefix} className={styles.item}>
      <span className={styles.name}>{user.name}</span>
      <span className={styles.lastSeen}>{formatLastSeen(user.lastSeen)}</span>
    </div>
  );

  return (
    <div className={styles.sidebar}>
      <div className={styles.header}>Users</div>

      {rooms.length > 0 && (
        <div className={styles.section}>
          <div className={styles.sectionTitle}>Rooms</div>
          {rooms.map(renderUser)}
        </div>
      )}

      {contacts.length > 0 && (
        <div className={styles.section}>
          <div className={styles.sectionTitle}>Contacts</div>
          {contacts.map(renderUser)}
        </div>
      )}

      {users.length === 0 && (
        <div className={styles.empty}>No users</div>
      )}
    </div>
  );
}
