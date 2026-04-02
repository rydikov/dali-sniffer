const messagesEl = document.getElementById('messages');
const formEl = document.getElementById('command-form');
const inputEl = document.getElementById('command-input');
const sendButtonEl = document.getElementById('send-button');
const stateEl = document.getElementById('ws-state');
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

function addMessage(kind, author, body, avatar, bubbleClass) {
  const row = document.createElement('article');
  row.className = 'msg ' + kind;
  row.innerHTML =
    '<div class="avatar">' + avatar + '</div>' +
    '<div class="bubble ' + (bubbleClass || '') + '">' +
      '<div class="meta">' +
        '<span class="name">' + escapeHtml(author) + '</span>' +
        '<span class="time">' + stamp() + '</span>' +
      '</div>' +
      '<div class="body">' + escapeHtml(body) + '</div>' +
    '</div>';
  messagesEl.appendChild(row);
  scrollToBottom();
}

function setState(text, online) {
  stateEl.textContent = text;
  stateEl.style.background = online ? 'rgba(94, 234, 212, 0.12)' : 'rgba(248, 113, 113, 0.12)';
  stateEl.style.color = online ? '#5eead4' : '#fda4af';
  sendButtonEl.disabled = !online;
}

function renderStatus(message) {
  const state = message.connected ? 'Connected' : 'Disconnected';
  const ssid = message.ssid || 'unknown';
  const ip = message.ip || 'not assigned';
  addMessage('status', 'Wi-Fi Status', state + ' | SSID: ' + ssid + ' | IP: ' + ip, '📡');
}

function renderAck(message) {
  const accepted = message.accepted ? 'accepted' : 'rejected';
  addMessage('command', 'Controller', 'Command "' + (message.command || '') + '" ' + accepted, '🤖');
}

function connect() {
  socket = new WebSocket(protocol + '://' + location.host + '/ws');
  setState('connecting', false);

  socket.addEventListener('open', () => {
    setState('online', true);
    addMessage('status', 'System', 'WebSocket connected. Waiting for Wi-Fi updates...', '✨');
  });

  socket.addEventListener('close', () => {
    setState('offline', false);
    addMessage('status', 'System', 'Connection lost. Retrying in 2 seconds...', '⚠️');
    setTimeout(connect, 2000);
  });

  socket.addEventListener('error', () => {
    setState('error', false);
  });

  socket.addEventListener('message', (event) => {
    let payload;
    try {
      payload = JSON.parse(event.data);
    } catch (err) {
      addMessage('status', 'System', 'Received invalid JSON payload', '⚠️');
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

  addMessage('self', 'You', command, '🧑', 'self');
  socket.send(JSON.stringify({ type: 'command', command }));
  inputEl.value = '';
  inputEl.focus();
});

addMessage('status', 'System', 'Open the page after ESP32 gets an IP address.', '💬');
connect();
