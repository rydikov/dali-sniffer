const messagesEl = document.getElementById('messages');
const formEl = document.getElementById('command-form');
const inputEl = document.getElementById('command-input');
const stateEl = document.getElementById('ws-state');
const statusDotEl = document.getElementById('status-dot');
const sendButtonEl = document.getElementById('send-button');
const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
let socket;

function escapeHtml(value) {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;');
}

function stamp() {
  return new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function scrollToBottom() {
  messagesEl.scrollTop = messagesEl.scrollHeight;
}

function classifyMessage(kind, body) {
  if (kind === 'self') {
    return 'command';
  }

  if (kind === 'command') {
    return body.includes('rejected') ? 'error' : 'success';
  }

  if (body.includes('Disconnected') || body.includes('lost')) {
    return 'warning';
  }

  if (body.includes('invalid') || body.includes('error')) {
    return 'error';
  }

  if (body.includes('Connected') || body.includes('Waiting') || body.includes('Open the page')) {
    return 'success';
  }

  return 'info';
}

function addMessage(kind, body) {
  const tone = classifyMessage(kind, body);
  const row = document.createElement('article');
  row.className = 'msg ' + kind;
  row.innerHTML =
    '<span class="time">[' + stamp() + ']</span>' +
    '<span class="body ' + tone + '">' + escapeHtml(body) + '</span>';
  messagesEl.appendChild(row);
  scrollToBottom();
}

function setState(text, online) {
  stateEl.textContent = text;
  stateEl.style.color = online ? '#9ec9b0' : '#d8d2de';
  statusDotEl.style.background = online ? '#9ec9b0' : '#8a8296';
  statusDotEl.style.boxShadow = online
    ? '0 0 0 4px rgba(158, 201, 176, 0.15)'
    : '0 0 0 4px rgba(138, 130, 150, 0.14)';
  sendButtonEl.disabled = !online;
}

function renderStatus(message) {
  const state = message.connected ? 'Connected' : 'Disconnected';
  const ssid = message.ssid || 'unknown';
  const ip = message.ip || 'not assigned';
  addMessage('status', state + ' | SSID: ' + ssid + ' | IP: ' + ip);
}

function renderAck(message) {
  const accepted = message.accepted ? 'accepted' : 'rejected';
  addMessage('command', 'Command "' + (message.command || '') + '" ' + accepted);
}

function connect() {
  socket = new WebSocket(protocol + '://' + location.host + '/ws');
  setState('connecting', false);

  socket.addEventListener('open', () => {
    setState('Connected', true);
    addMessage('status', 'WebSocket connected. Waiting for Wi-Fi updates...');
  });

  socket.addEventListener('close', () => {
    setState('Offline', false);
    addMessage('status', 'Connection lost. Retrying in 2 seconds...');
    setTimeout(connect, 2000);
  });

  socket.addEventListener('error', () => {
    setState('Error', false);
  });

  socket.addEventListener('message', (event) => {
    let payload;
    try {
      payload = JSON.parse(event.data);
    } catch (err) {
      addMessage('status', 'Error: received invalid JSON payload');
      return;
    }

    if (payload.type === 'wifi_status') {
      renderStatus(payload);
    } else if (payload.type === 'command_ack') {
      renderAck(payload);
    }
  });
}

formEl.addEventListener('submit', (event) => {
  event.preventDefault();
  const command = inputEl.value.trim();
  if (!command || !socket || socket.readyState !== WebSocket.OPEN) {
    return;
  }

  addMessage('self', command);
  socket.send(JSON.stringify({ type: 'command', command }));
  inputEl.value = '';
  inputEl.focus();
});

addMessage('status', 'Server started successfully');
addMessage('status', 'Open the page after ESP32 gets an IP address.');
connect();
