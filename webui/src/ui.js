const messagesEl = document.getElementById('messages');
const formEl = document.getElementById('command-form');
const inputEl = document.getElementById('command-input');
const ghostEl = document.getElementById('command-ghost');
const stateEl = document.getElementById('ws-state');
const statusDotEl = document.getElementById('status-dot');
const sendButtonEl = document.getElementById('send-button');
const pauseButtonEl = document.getElementById('pause-button');
const clearButtonEl = document.getElementById('clear-button');

function stamp() {
  return new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function classifyMessage(kind, body) {
  if (kind === 'self') {
    return 'command';
  }

  if (kind === 'command') {
    return body.includes('rejected') ? 'error' : 'success';
  }

  if (body.includes('Disconnected') || body.includes('lost') || body.includes('Retrying')) {
    return 'warning';
  }

  if (body.includes('invalid') || body.includes('Error')) {
    return 'error';
  }

  if (body.includes('Connected') || body.includes('Waiting') || body.includes('started')) {
    return 'success';
  }

  return 'info';
}

export function addMessage(kind, body) {
  const row = document.createElement('article');
  const time = document.createElement('span');
  const message = document.createElement('span');

  row.className = `msg ${kind}`;
  time.className = 'time';
  message.className = `body ${classifyMessage(kind, body)}`;

  time.textContent = `[${stamp()}]`;
  message.textContent = body;

  row.append(time, message);
  messagesEl.appendChild(row);
  messagesEl.scrollTop = messagesEl.scrollHeight;
}

export function setConnectionState(text, isOnline) {
  stateEl.textContent = text;
  stateEl.style.color = isOnline ? '#9ec9b0' : '#d8d2de';
  statusDotEl.style.background = isOnline ? '#9ec9b0' : '#8a8296';
  statusDotEl.style.boxShadow = isOnline
    ? '0 0 0 2px rgba(158, 201, 176, 0.14)'
    : '0 0 0 2px rgba(138, 130, 150, 0.14)';
  sendButtonEl.disabled = !isOnline;
}

export function renderMessage(payload) {
  const value = payload.value || '';
  addMessage('status', `Message: ${value}`);
}

export function renderAckMessage(payload) {
  const accepted = payload.accepted ? 'accepted' : 'rejected';
  addMessage('command', `Command "${payload.command || ''}" ${accepted}`);
}

export function registerComposerHandlers({ onSubmit, onAutocomplete }) {
  function syncSuggestion() {
    if (document.activeElement !== inputEl) {
      ghostEl.textContent = '';
      return;
    }

    const suggestion = onAutocomplete(inputEl.value);
    const typedValue = inputEl.value;

    if (!suggestion || suggestion.length <= typedValue.length) {
      ghostEl.textContent = '';
      return;
    }

    ghostEl.textContent = `${typedValue}${suggestion.slice(typedValue.length)}`;
  }

  formEl.addEventListener('submit', (event) => {
    event.preventDefault();

    const command = inputEl.value.trim();
    if (!command) {
      return;
    }

    onSubmit(command);
  });

  inputEl.addEventListener('input', syncSuggestion);
  inputEl.addEventListener('focus', syncSuggestion);
  inputEl.addEventListener('blur', () => {
    ghostEl.textContent = '';
  });

  inputEl.addEventListener('keydown', (event) => {
    if (event.key !== 'Tab' || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey) {
      return;
    }

    const suggestion = onAutocomplete(inputEl.value);
    if (!suggestion) {
      return;
    }

    event.preventDefault();
    inputEl.value = suggestion;
    inputEl.setSelectionRange(suggestion.length, suggestion.length);
    syncSuggestion();
  });

  syncSuggestion();
}

export function clearCommandInput() {
  inputEl.value = '';
  ghostEl.textContent = '';
  inputEl.focus();
}

export function clearMessages() {
  messagesEl.replaceChildren();
}

export function setPauseState(isPaused) {
  pauseButtonEl.classList.toggle('is-active', isPaused);
  pauseButtonEl.setAttribute('aria-pressed', isPaused ? 'true' : 'false');
  pauseButtonEl.setAttribute('title', isPaused ? 'Resume updates' : 'Pause updates');
}

export function registerToolbarHandlers({ onPauseToggle, onClear }) {
  pauseButtonEl.addEventListener('click', onPauseToggle);
  clearButtonEl.addEventListener('click', onClear);
}
