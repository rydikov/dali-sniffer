const WS_STORAGE_KEY = 'esp32-ws-url';

function normalizeWsTarget(value) {
  if (!value) {
    return '';
  }

  if (value.startsWith('ws://') || value.startsWith('wss://')) {
    return value;
  }

  return `ws://${value.replace(/\/+$/, '')}/ws`;
}

export function resolveWebSocketUrl() {
  const params = new URLSearchParams(window.location.search);
  const queryValue = params.get('ws');
  const storedValue = window.localStorage.getItem(WS_STORAGE_KEY);
  const override = queryValue || storedValue;

  if (override) {
    const normalized = normalizeWsTarget(override);

    if (queryValue) {
      window.localStorage.setItem(WS_STORAGE_KEY, queryValue);
    }

    return normalized;
  }

  const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
  return `${protocol}://${window.location.host}/ws`;
}

export function getDevConnectionHint() {
  const isLocalDevHost =
    window.location.hostname === 'localhost' ||
    window.location.hostname === '127.0.0.1';

  if (!isLocalDevHost) {
    return '';
  }

  return 'Running via Vite dev server. Add ?ws=DEVICE_IP:80 to connect to the ESP32.';
}
