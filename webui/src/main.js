import './styles.css';

import { getAutocompleteSuggestion, getHelpHtml, isHelpCommand } from './commands.js';
import { getDevConnectionHint, resolveWebSocketUrl } from './config.js';
import {
  addHtmlMessage,
  addMessage,
  clearMessages,
  clearCommandInput,
  registerToolbarHandlers,
  registerComposerHandlers,
  renderAckMessage,
  renderMessage,
  setPauseState,
  setConnectionState
} from './ui.js';
import { createSocketClient } from './ws.js';

const consolePageEl = document.getElementById('console-page');
const devicesPageEl = document.getElementById('devices-page');
const deviceDialogEl = document.getElementById('device-dialog');
const deviceFormEl = document.getElementById('device-form');
const deviceAddressEl = document.getElementById('device-address');
const deviceBrightnessEl = document.getElementById('device-brightness');
const deviceColorTemperatureEl = document.getElementById('device-color-temperature');
const deviceRgbwEl = document.getElementById('device-rgbw');
const deviceGroupsEl = document.getElementById('device-groups');
const deviceScenesEl = document.getElementById('device-scenes');
const devicesStatusEl = document.getElementById('devices-status');
const deviceFormStatusEl = document.getElementById('device-form-status');
const deviceDialogTitleEl = document.getElementById('device-dialog-title');
const openDeviceDialogEl = document.getElementById('open-device-dialog');
const closeDeviceDialogEl = document.getElementById('close-device-dialog');
const cancelDeviceButtonEl = document.getElementById('cancel-device-button');
const addDeviceButtonEl = document.getElementById('add-device-button');
const devicesListEl = document.getElementById('devices-list');
const devicesCountEl = document.getElementById('devices-count');
let isPaused = false;
let isConsoleInitialized = false;
let isDevicesInitialized = false;
let editingDeviceAddress = null;
let devices = [];

function pushMessage(kind, body) {
  if (isPaused && kind !== 'self') {
    return;
  }

  addMessage(kind, body);
}

function showRoute(pathname) {
  const route = pathname === '/devices' ? '/devices' : '/';
  const isDevices = route === '/devices';

  consolePageEl.hidden = isDevices;
  devicesPageEl.hidden = !isDevices;
  document.title = isDevices ? 'DALI Devices' : 'ESP32 Wi-Fi Console';

  if (!isDevices && !isConsoleInitialized) {
    initConsole();
  }
  if (isDevices) {
    initDevices();
    fetchDevices();
  }
}

function navigate(pathname) {
  const route = pathname === '/devices' ? '/devices' : '/';

  if (window.location.pathname !== route) {
    window.history.pushState({}, '', route);
  }

  showRoute(route);
}

function registerRouter() {
  document.addEventListener('click', (event) => {
    if (!(event.target instanceof Element)) {
      return;
    }

    const link = event.target.closest('a[data-route]');
    if (!link) {
      return;
    }

    const url = new URL(link.href);
    if (url.origin !== window.location.origin) {
      return;
    }

    event.preventDefault();
    navigate(url.pathname);
  });

  window.addEventListener('popstate', () => {
    showRoute(window.location.pathname);
  });
}

function initConsole() {
  isConsoleInitialized = true;

  const socketUrl = resolveWebSocketUrl();
  const devHint = getDevConnectionHint();

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
      pushMessage('status', 'WebSocket connected. Waiting for messages...');
    },
    onClose() {
      setConnectionState('Offline', false);
      pushMessage('status', 'Connection lost. Retrying in 2 seconds...');
    },
    onError() {
      setConnectionState('Error', false);
    },
    onMessage(payload) {
      if (!isPaused) {
        renderMessage(payload);
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

  registerComposerHandlers({
    onSubmit(command) {
      if (isHelpCommand(command)) {
        addHtmlMessage('status', getHelpHtml(), 'help');
        clearCommandInput();
        return;
      }

      if (!client.sendCommand(command)) {
        return;
      }

      addMessage('self', command);
      clearCommandInput();
    },
    onAutocomplete(input) {
      return getAutocompleteSuggestion(input);
    }
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
}

function createNumberOption(value) {
  const option = document.createElement('option');
  option.value = String(value);
  option.textContent = String(value);
  return option;
}

function createCheckbox(name, value) {
  const label = document.createElement('label');
  const input = document.createElement('input');
  const text = document.createElement('span');

  label.className = 'check-tile';
  input.type = 'checkbox';
  input.name = name;
  input.value = String(value);
  text.textContent = String(value);

  label.append(input, text);
  return label;
}

function initDevices() {
  if (isDevicesInitialized) {
    return;
  }

  isDevicesInitialized = true;

  for (let i = 0; i <= 63; i += 1) {
    deviceAddressEl.appendChild(createNumberOption(i));
  }
  for (let i = 0; i <= 15; i += 1) {
    deviceGroupsEl.appendChild(createCheckbox('groups', i));
    deviceScenesEl.appendChild(createCheckbox('scenes', i));
  }

  deviceFormEl.addEventListener('submit', async (event) => {
    event.preventDefault();
    const device = readDeviceForm();
    if (editingDeviceAddress === null) {
      await createDevice(device);
      return;
    }

    await updateDevice(device);
  });

  openDeviceDialogEl.addEventListener('click', () => {
    openCreateDeviceDialog();
  });
  closeDeviceDialogEl.addEventListener('click', () => {
    closeDeviceDialog();
  });
  cancelDeviceButtonEl.addEventListener('click', () => {
    closeDeviceDialog();
  });
}

function setDevicesStatus(message, kind = 'info') {
  devicesStatusEl.textContent = message;
  devicesStatusEl.className = `devices-status ${kind}`;
}

function setDeviceFormStatus(message, kind = 'info') {
  deviceFormStatusEl.textContent = message;
  deviceFormStatusEl.className = `form-status ${kind}`;
}

function openDeviceDialog() {
  setDeviceFormStatus('');
  if (typeof deviceDialogEl.showModal === 'function') {
    deviceDialogEl.showModal();
  } else {
    deviceDialogEl.setAttribute('open', '');
  }

  const focusTarget = deviceAddressEl.disabled ? deviceBrightnessEl : deviceAddressEl;
  focusTarget.focus();
}

function openCreateDeviceDialog() {
  editingDeviceAddress = null;
  resetDeviceForm();
  deviceAddressEl.disabled = false;
  deviceDialogTitleEl.textContent = 'New device';
  addDeviceButtonEl.textContent = 'Add';
  openDeviceDialog();
}

function openEditDeviceDialog(device) {
  editingDeviceAddress = device.address;
  setDeviceForm(device);
  deviceAddressEl.disabled = true;
  deviceDialogTitleEl.textContent = `Edit address ${device.address}`;
  addDeviceButtonEl.textContent = 'Save';
  openDeviceDialog();
}

function closeDeviceDialog() {
  setDeviceFormStatus('');
  if (typeof deviceDialogEl.close === 'function') {
    deviceDialogEl.close();
  } else {
    deviceDialogEl.removeAttribute('open');
  }
}

function getCheckedNumbers(name) {
  return Array.from(deviceFormEl.querySelectorAll(`input[name="${name}"]:checked`)).map((input) => Number(input.value));
}

function setCheckedNumbers(name, values) {
  const selected = new Set(Array.isArray(values) ? values.map((value) => Number(value)) : []);
  for (const input of deviceFormEl.querySelectorAll(`input[name="${name}"]`)) {
    input.checked = selected.has(Number(input.value));
  }
}

function readDeviceForm() {
  return {
    address: Number(deviceAddressEl.value),
    brightness: deviceBrightnessEl.checked,
    colorTemperature: deviceColorTemperatureEl.checked,
    rgbw: deviceRgbwEl.checked,
    groups: getCheckedNumbers('groups'),
    scenes: getCheckedNumbers('scenes')
  };
}

function resetDeviceForm() {
  deviceFormEl.reset();
  deviceAddressEl.value = '0';
  setCheckedNumbers('groups', []);
  setCheckedNumbers('scenes', []);
}

function setDeviceForm(device) {
  resetDeviceForm();
  deviceAddressEl.value = String(device.address);
  deviceBrightnessEl.checked = Boolean(device.brightness);
  deviceColorTemperatureEl.checked = Boolean(device.colorTemperature);
  deviceRgbwEl.checked = Boolean(device.rgbw);
  setCheckedNumbers('groups', device.groups);
  setCheckedNumbers('scenes', device.scenes);
}

function capabilityLabels(device) {
  const labels = [];
  if (device.brightness) {
    labels.push('Brightness');
  }
  if (device.colorTemperature) {
    labels.push('Color temperature');
  }
  if (device.rgbw) {
    labels.push('RGBW');
  }

  return labels;
}

function numberList(values) {
  return Array.isArray(values) && values.length > 0 ? values.join(', ') : 'none';
}

function createLampIcon() {
  const icon = document.createElement('div');
  icon.className = 'device-icon';
  icon.innerHTML = [
    '<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="24" height="24" fill="none" viewBox="0 0 24 24">',
    '<path stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 18h6m-5 3h4m-7-7a7 7 0 1 1 10 0c-.93.87-1.5 1.96-1.5 3.25h-7C8.5 15.96 7.93 14.87 7 14Z"/>',
    '</svg>'
  ].join('');
  return icon;
}

function renderDeviceCard(device) {
  const card = document.createElement('article');
  const header = document.createElement('div');
  const title = document.createElement('h3');
  const meta = document.createElement('div');
  const actions = document.createElement('div');
  const editButton = document.createElement('button');
  const deleteButton = document.createElement('button');
  const capabilities = capabilityLabels(device);

  card.className = 'device-card';
  header.className = 'device-card-header';
  meta.className = 'device-card-meta';
  title.textContent = `Address ${device.address}`;
  actions.className = 'device-card-actions';
  editButton.className = 'secondary-button compact-button';
  editButton.type = 'button';
  editButton.textContent = 'Edit';
  deleteButton.className = 'secondary-button compact-button danger-button';
  deleteButton.type = 'button';
  deleteButton.textContent = 'Delete';
  editButton.addEventListener('click', () => {
    openEditDeviceDialog(device);
  });
  deleteButton.addEventListener('click', () => {
    deleteDevice(device.address);
  });
  actions.append(editButton, deleteButton);

  header.append(createLampIcon(), title);

  const rows = [
    ['Capabilities', capabilities.length > 0 ? capabilities.join(', ') : 'none selected'],
    ['Groups', numberList(device.groups)],
    ['Scenes', numberList(device.scenes)]
  ];

  for (const [label, value] of rows) {
    const row = document.createElement('p');
    const labelEl = document.createElement('span');
    const valueEl = document.createElement('strong');

    labelEl.textContent = label;
    valueEl.textContent = value;
    row.append(labelEl, valueEl);
    meta.appendChild(row);
  }

  card.append(header, meta, actions);
  return card;
}

function renderDevices() {
  devicesListEl.replaceChildren();
  devicesCountEl.textContent = String(devices.length);

  if (devices.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'devices-empty';
    empty.textContent = 'No saved devices yet.';
    devicesListEl.appendChild(empty);
    return;
  }

  for (const device of devices) {
    devicesListEl.appendChild(renderDeviceCard(device));
  }
}

async function fetchDevices() {
  setDevicesStatus('Loading devices...', 'info');

  try {
    const response = await fetch('/api/devices', { headers: { Accept: 'application/json' } });
    if (!response.ok) {
      throw new Error('load_failed');
    }

    const payload = await response.json();
    devices = Array.isArray(payload.devices) ? payload.devices : [];
    renderDevices();
    setDevicesStatus(devices.length > 0 ? 'Device list updated.' : 'Add the first device.', 'success');
  } catch {
    setDevicesStatus('Failed to load devices.', 'error');
    renderDevices();
  }
}

async function createDevice(device) {
  addDeviceButtonEl.disabled = true;
  setDeviceFormStatus('Saving device...', 'info');

  try {
    const response = await fetch('/api/devices', {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(device)
    });

    if (response.status === 409) {
      setDeviceFormStatus('Address is already used.', 'error');
      return;
    }
    if (!response.ok) {
      setDeviceFormStatus('Failed to save device.', 'error');
      return;
    }

    const payload = await response.json();
    if (payload.device) {
      devices = [...devices, payload.device].sort((a, b) => a.address - b.address);
      renderDevices();
    }

    resetDeviceForm();
    closeDeviceDialog();
    setDevicesStatus('Device added.', 'success');
  } catch {
    setDeviceFormStatus('Failed to send request.', 'error');
  } finally {
    addDeviceButtonEl.disabled = false;
  }
}

async function updateDevice(device) {
  addDeviceButtonEl.disabled = true;
  setDeviceFormStatus('Saving device...', 'info');

  try {
    const response = await fetch(`/api/devices/${device.address}`, {
      method: 'PUT',
      headers: {
        Accept: 'application/json',
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(device)
    });

    if (response.status === 404) {
      setDeviceFormStatus('Device was not found.', 'error');
      return;
    }
    if (!response.ok) {
      setDeviceFormStatus('Failed to save device.', 'error');
      return;
    }

    const payload = await response.json();
    if (payload.device) {
      devices = devices.map((item) => (item.address === payload.device.address ? payload.device : item));
      renderDevices();
    }

    closeDeviceDialog();
    setDevicesStatus('Device updated.', 'success');
  } catch {
    setDeviceFormStatus('Failed to send request.', 'error');
  } finally {
    addDeviceButtonEl.disabled = false;
  }
}

async function deleteDevice(address) {
  if (!window.confirm(`Delete device at address ${address}?`)) {
    return;
  }

  setDevicesStatus('Deleting device...', 'info');

  try {
    const response = await fetch(`/api/devices/${address}`, {
      method: 'DELETE',
      headers: { Accept: 'application/json' }
    });

    if (response.status === 404) {
      setDevicesStatus('Device was not found.', 'error');
      return;
    }
    if (!response.ok) {
      setDevicesStatus('Failed to delete device.', 'error');
      return;
    }

    devices = devices.filter((device) => device.address !== address);
    renderDevices();
    setDevicesStatus('Device deleted.', 'success');
  } catch {
    setDevicesStatus('Failed to send request.', 'error');
  }
}

registerRouter();
showRoute(window.location.pathname);
