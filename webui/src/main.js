import './styles.css';

import { getDevConnectionHint, resolveWebSocketUrl } from './config.js';
import {
  addMessage,
  clearMessages,
  clearCommandInput,
  registerToolbarHandlers,
  registerCommandHandler,
  renderAckMessage,
  renderTimeMessage,
  setPauseState,
  setConnectionState
} from './ui.js';
import { createSocketClient } from './ws.js';

const socketUrl = resolveWebSocketUrl();
const devHint = getDevConnectionHint();
let isPaused = false;

function pushMessage(kind, body) {
  if (isPaused && kind !== 'self') {
    return;
  }

  addMessage(kind, body);
}

pushMessage('status', 'Server started successfully');
if (devHint) {
  pushMessage('status', devHint);
} else {
  pushMessage('status', 'Open the page after ESP32 gets an IP address.');
}

const client = createSocketClient({
  url: socketUrl,
  onOpen(_, opened) {
    if (!opened) {
      setConnectionState('Connecting', false);
      return;
    }

    setConnectionState('Connected', true);
    pushMessage('status', 'WebSocket connected. Waiting for time updates...');
  },
  onClose() {
    setConnectionState('Offline', false);
    pushMessage('status', 'Connection lost. Retrying in 2 seconds...');
  },
  onError() {
    setConnectionState('Error', false);
  },
  onTime(payload) {
    if (!isPaused) {
      renderTimeMessage(payload);
    }
  },
  onAck(payload) {
    if (!isPaused) {
      renderAckMessage(payload);
    }
  },
  onInvalidPayload() {
    pushMessage('status', 'Error: received invalid JSON payload');
  }
});

registerCommandHandler((command) => {
  if (!client.sendCommand(command)) {
    return;
  }

  addMessage('self', command);
  clearCommandInput();
});

registerToolbarHandlers({
  onPauseToggle() {
    isPaused = !isPaused;
    setPauseState(isPaused);
    addMessage('status', isPaused ? 'Updates paused' : 'Updates resumed');
  },
  onClear() {
    clearMessages();
    if (!isPaused) {
      addMessage('status', 'Chat cleared');
    }
  }
});

setPauseState(false);
