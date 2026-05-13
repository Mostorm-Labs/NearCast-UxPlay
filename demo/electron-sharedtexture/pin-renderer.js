const { ipcRenderer } = require('electron');

const pinValue = document.getElementById('pinValue');

if (!ipcRenderer || !pinValue) {
  throw new Error('PIN renderer failed to initialize.');
}

function applyStatus(status) {
  const pin = typeof status?.pin === 'string' && /^\d{4}$/.test(status.pin)
    ? status.pin
    : null;
  pinValue.textContent = pin || '----';
}

ipcRenderer.on('uxplay-control:status', (_event, status) => {
  applyStatus(status);
});
