import { useState, useEffect, useRef } from 'react';
import { api, Channel, Message } from '../api';
import { MessageList } from './MessageList';
import { MessageInput } from './MessageInput';
import { LoginDialog } from './LoginDialog';
import styles from './ChatPane.module.css';

interface ChatPaneProps {
  channel: Channel;
}

export function ChatPane({ channel }: ChatPaneProps) {
  const [messages, setMessages] = useState<Message[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [showLoginDialog, setShowLoginDialog] = useState(channel.locked && !channel.joined);
  const [noticeMsg, setNoticeMsg] = useState<string | null>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  // Load messages when channel changes
  useEffect(() => {
    setLoading(true);
    setShowLoginDialog(channel.locked && !channel.joined);

    const loadMessages = async () => {
      try {
        const newMessages = await api.getMessages(channel.id, 100);
        setMessages(newMessages);
        setError(null);
      } catch (e) {
        setError(e instanceof Error ? e.message : 'Failed to load messages');
      } finally {
        setLoading(false);
      }
    };

    loadMessages();
  }, [channel.id, channel.locked, channel.joined]);

  // Listen for incoming messages
  useEffect(() => {
    const handleMessage = (event: any) => {
      if (event.message.channelId === channel.id) {
        setMessages((prev) => {
          // Update existing message or add new one
          const existingIdx = prev.findIndex((m) => m.id === event.message.id);
          if (existingIdx >= 0) {
            const updated = [...prev];
            updated[existingIdx] = event.message;
            return updated;
          } else {
            return [...prev, event.message];
          }
        });
      }
    };

    const handleLogin = (event: any) => {
      if (event.channelId === channel.id) {
        setShowLoginDialog(false);
        if (!event.ok) {
          setError(event.error || 'Login failed');
        }
      }
    };

    const handleNotice = (event: any) => {
      setNoticeMsg(event.text);
    };

    api.on('message', handleMessage);
    api.on('login', handleLogin);
    api.on('notice', handleNotice);

    return () => {
      api.off('message', handleMessage);
      api.off('login', handleLogin);
      api.off('notice', handleNotice);
    };
  }, [channel.id]);

  // Clear notice message after 5 seconds
  useEffect(() => {
    if (noticeMsg) {
      const timer = setTimeout(() => setNoticeMsg(null), 5000);
      return () => clearTimeout(timer);
    }
  }, [noticeMsg]);

  // Auto-scroll to bottom
  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages]);

  const handleSendMessage = async (text: string) => {
    try {
      const result = await api.sendMessage(channel.id, text);
      if (!result.ok) {
        setError('Failed to send message');
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to send message');
    }
  };

  const handleJoin = async (password?: string) => {
    try {
      await api.joinChannel(channel.id, password);
      setShowLoginDialog(false);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to join channel');
    }
  };

  if (showLoginDialog && channel.kind === 'room') {
    return (
      <div className={styles.container}>
        <div className={styles.header}>
          <h2>{channel.displayName}</h2>
        </div>
        <div className={styles.content}>
          <LoginDialog
            roomName={channel.name}
            onLogin={handleJoin}
          />
        </div>
      </div>
    );
  }

  return (
    <div className={styles.container}>
      <div className={styles.header}>
        <h2>{channel.displayName}</h2>
        {channel.kind === 'room' && <span className={styles.badge}>Room</span>}
      </div>

      <div className={styles.messages}>
        {noticeMsg && <div className={styles.error}>{noticeMsg}</div>}
        {error && <div className={styles.error}>{error}</div>}
        {loading ? (
          <div className={styles.loading}>Loading messages...</div>
        ) : messages.length === 0 ? (
          <div className={styles.empty}>No messages yet. Start a conversation!</div>
        ) : (
          <MessageList messages={messages} />
        )}
        <div ref={messagesEndRef} />
      </div>

      <MessageInput onSendMessage={handleSendMessage} disabled={!channel.joined} />
    </div>
  );
}
