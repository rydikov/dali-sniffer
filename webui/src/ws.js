export function createSocketClient({ url, onOpen, onClose, onError, onMessage, onAck, onInvalidPayload }) {
  let socket;

  function connect() {
    socket = new WebSocket(url);

    onOpen(socket);

    socket.addEventListener('open', () => {
      onOpen(socket, true);
    });

    socket.addEventListener('close', () => {
      onClose();
      window.setTimeout(connect, 2000);
    });

    socket.addEventListener('error', () => {
      onError();
    });

    socket.addEventListener('message', (event) => {
      let payload;

      try {
        payload = JSON.parse(event.data);
      } catch (error) {
        onInvalidPayload();
        return;
      }

      if (payload.type === 'message') {
        onMessage(payload);
      } else if (payload.type === 'command_ack') {
        onAck(payload);
      }
    });
  }

  function sendCommand(command) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      return false;
    }

    socket.send(JSON.stringify({ type: 'command', command }));
    return true;
  }

  connect();

  return {
    sendCommand
  };
}
