import { useState } from 'react';
import styles from './LoginDialog.module.css';

interface LoginDialogProps {
  roomName: string;
  onLogin: (password?: string) => void;
}

export function LoginDialog({ roomName, onLogin }: LoginDialogProps) {
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    onLogin(password || undefined);
  };

  return (
    <div className={styles.dialog}>
      <div className={styles.content}>
        <h2>🔒 Join Locked Room</h2>
        <p>This room requires a password to join.</p>

        <form onSubmit={handleSubmit}>
          <div className={styles.field}>
            <label htmlFor="room">Room</label>
            <input
              id="room"
              type="text"
              value={roomName}
              disabled
              className={styles.input}
            />
          </div>

          <div className={styles.field}>
            <label htmlFor="password">Password</label>
            <div className={styles.passwordInput}>
              <input
                id="password"
                type={showPassword ? 'text' : 'password'}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Enter password"
                className={styles.input}
                autoFocus
              />
              <button
                type="button"
                className={styles.togglePassword}
                onClick={() => setShowPassword(!showPassword)}
              >
                {showPassword ? '👁️' : '👁️‍🗨️'}
              </button>
            </div>
          </div>

          <div className={styles.actions}>
            <button type="submit" className={styles.joinButton}>
              Join Room
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
