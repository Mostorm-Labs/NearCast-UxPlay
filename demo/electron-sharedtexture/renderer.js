const { ipcRenderer, sharedTexture } = require('electron');

if (!sharedTexture) {
  throw new Error('sharedTexture is unavailable in this renderer.');
}
if (!ipcRenderer) {
  throw new Error('ipcRenderer is unavailable in this renderer.');
}

const canvas = document.getElementById('videoCanvas');
const statusBadge = document.getElementById('statusBadge');
const stateValue = document.getElementById('stateValue');
const sizeValue = document.getElementById('sizeValue');
const frameValue = document.getElementById('frameValue');
const timestampValue = document.getElementById('timestampValue');
const textureValue = document.getElementById('textureValue');
const probeValue = document.getElementById('probeValue');
const pinPortValue = document.getElementById('pinPortValue');
const pinForm = document.getElementById('pinForm');
const pinInput = document.getElementById('pinInput');
const pinSubmitButton = document.getElementById('pinSubmitButton');
const pinFeedback = document.getElementById('pinFeedback');
const muteStateValue = document.getElementById('muteStateValue');
const muteToggleButton = document.getElementById('muteToggleButton');
const muteFeedback = document.getElementById('muteFeedback');
const context = canvas.getContext('2d', { alpha: false });
if (
  !pinPortValue ||
  !pinForm ||
  !pinInput ||
  !pinSubmitButton ||
  !pinFeedback ||
  !muteStateValue ||
  !muteToggleButton ||
  !muteFeedback
) {
  throw new Error('Control UI elements are missing.');
}
const query =
  typeof window !== 'undefined' ? new URLSearchParams(window.location.search) : new URLSearchParams();
const enableProbe = query.get('probe') === '1';
const renderTimeoutMs = Number.parseInt(query.get('renderTimeoutMs') || '250', 10) || 0;
let probeFramesRemaining = enableProbe ? 5 : 0;
let drawInFlight = false;
let pendingPacket = null;
let controlSessionToken = null;
let currentMuted = null;

function setPinFeedback(message, tone = 'muted') {
  pinFeedback.textContent = message;
  pinFeedback.dataset.tone = tone;
}

function setPinControlEnabled(enabled) {
  pinSubmitButton.disabled = !enabled;
}

function setMuteFeedback(message, tone = 'muted') {
  muteFeedback.textContent = message;
  muteFeedback.dataset.tone = tone;
}

function setMuteControlEnabled(enabled) {
  muteToggleButton.disabled = !enabled;
}

function updatePortDisplay(port) {
  pinPortValue.textContent = Number.isInteger(port) && port > 0 ? String(port) : '-';
}

function updatePinDisplay(pin) {
  pinInput.value = typeof pin === 'string' && /^\d{4}$/.test(pin) ? pin : '----';
}

function updateMuteDisplay(muted) {
  currentMuted = typeof muted === 'boolean' ? muted : null;
  if (currentMuted === true) {
    muteStateValue.textContent = 'Muted';
    muteToggleButton.textContent = 'Unmute';
    return;
  }
  if (currentMuted === false) {
    muteStateValue.textContent = 'Unmuted';
    muteToggleButton.textContent = 'Mute';
    return;
  }
  muteStateValue.textContent = 'Unknown';
  muteToggleButton.textContent = 'Mute';
}

function applyControlStatus(status) {
  const port = Number.isInteger(status?.port) ? status.port : null;
  const pin = typeof status?.pin === 'string' ? status.pin : null;
  const rotating = status?.rotating === true;
  const audioUpdating = status?.audioUpdating === true;
  const muted = typeof status?.muted === 'boolean'
    ? status.muted
    : (typeof status?.mirrorAudioEnabled === 'boolean' ? !status.mirrorAudioEnabled : null);
  const hasPort = Boolean(port && port > 0);

  updatePortDisplay(port);
  updatePinDisplay(pin);
  updateMuteDisplay(muted);
  setPinControlEnabled(hasPort && !rotating);
  setMuteControlEnabled(hasPort && !audioUpdating);

  if (!hasPort) {
    setPinFeedback('UxPlay started, waiting for control port...', 'muted');
  } else if (rotating) {
    setPinFeedback('Updating random PIN...', 'muted');
  } else if (pin) {
    setPinFeedback(`Current PIN: ${pin}`, 'muted');
  } else {
    setPinFeedback('Waiting for first mirroring session to generate PIN...', 'muted');
  }

  if (!hasPort) {
    setMuteFeedback('UxPlay started, waiting for control port...', 'muted');
  } else if (audioUpdating) {
    setMuteFeedback('Applying audio mute setting...', 'muted');
  } else if (typeof muted === 'boolean') {
    setMuteFeedback(`Audio is currently ${muted ? 'muted' : 'unmuted'}.`, 'muted');
  } else {
    setMuteFeedback('Waiting for audio status from /api/audio...', 'muted');
  }
}

async function initializePinControl() {
  setPinControlEnabled(false);
  setMuteControlEnabled(false);
  updatePinDisplay(null);
  updateMuteDisplay(null);
  try {
    const session = await ipcRenderer.invoke('uxplay-control:get-session');
    if (!session?.ok) {
      setPinFeedback(`PIN control unavailable: ${session?.error?.message || 'unknown error'}`, 'error');
      setMuteFeedback(`Audio control unavailable: ${session?.error?.message || 'unknown error'}`, 'error');
      return;
    }
    controlSessionToken = session.token;
    applyControlStatus(session);
  } catch (error) {
    setPinFeedback(`PIN control init failed: ${error.message}`, 'error');
    setMuteFeedback(`Audio control init failed: ${error.message}`, 'error');
  }
}

async function submitPinUpdate(event) {
  event.preventDefault();
  try {
    const latestSession = await ipcRenderer.invoke('uxplay-control:get-session');
    if (latestSession?.ok) {
      controlSessionToken = latestSession.token;
      applyControlStatus(latestSession);
    }
  } catch {
    // Keep last known session token and port.
  }

  if (!controlSessionToken) {
    setPinFeedback('Control session is not initialized.', 'error');
    return;
  }
  if (pinPortValue.textContent === '-') {
    setPinFeedback('Control port is unavailable.', 'error');
    return;
  }

  setPinControlEnabled(false);
  setPinFeedback('Updating random PIN...', 'muted');
  try {
    const response = await ipcRenderer.invoke('uxplay-control:rotate-pin', {
      token: controlSessionToken,
    });

    if (!response?.ok) {
      setPinFeedback(
        `PIN update failed [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    applyControlStatus(response);
    setPinFeedback(
      `PIN updated to ${response?.pin}. ${response?.result?.message || 'UxPlay accepted the change.'}`,
      'success',
    );
  } catch (error) {
    setPinFeedback(`PIN update request failed: ${error.message}`, 'error');
  } finally {
    try {
      const latestSession = await ipcRenderer.invoke('uxplay-control:get-session');
      if (latestSession?.ok) {
        applyControlStatus(latestSession);
      } else {
        setPinControlEnabled(true);
      }
    } catch {
      setPinControlEnabled(true);
    }
  }
}

async function submitMuteToggle() {
  try {
    const latestSession = await ipcRenderer.invoke('uxplay-control:get-session');
    if (latestSession?.ok) {
      controlSessionToken = latestSession.token;
      applyControlStatus(latestSession);
    }
  } catch {
    // Keep last known session token and status.
  }

  if (!controlSessionToken) {
    setMuteFeedback('Control session is not initialized.', 'error');
    return;
  }
  if (pinPortValue.textContent === '-') {
    setMuteFeedback('Control port is unavailable.', 'error');
    return;
  }

  const targetMuted = currentMuted === null ? true : !currentMuted;
  setMuteControlEnabled(false);
  setMuteFeedback(`${targetMuted ? 'Muting' : 'Unmuting'} mirror audio...`, 'muted');
  try {
    const response = await ipcRenderer.invoke('uxplay-control:set-muted', {
      token: controlSessionToken,
      muted: targetMuted,
    });

    if (!response?.ok) {
      setMuteFeedback(
        `Audio update failed [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    updateMuteDisplay(response?.muted);
    setMuteFeedback(
      `${response?.muted ? 'Muted' : 'Unmuted'} successfully. ${response?.result?.message || 'UxPlay accepted the change.'}`,
      'success',
    );
  } catch (error) {
    setMuteFeedback(`Audio update request failed: ${error.message}`, 'error');
  } finally {
    try {
      const latestSession = await ipcRenderer.invoke('uxplay-control:get-session');
      if (latestSession?.ok) {
        applyControlStatus(latestSession);
      } else {
        setMuteControlEnabled(true);
      }
    } catch {
      setMuteControlEnabled(true);
    }
  }
}

function resizeCanvas(width, height) {
  if (canvas.width === width && canvas.height === height) {
    return;
  }

  canvas.width = width;
  canvas.height = height;
}

async function drawFrame(frame, width, height) {
  if (typeof createImageBitmap === 'function') {
    try {
      const bitmap = await createImageBitmap(frame);
      try {
        context.drawImage(bitmap, 0, 0, width, height);
        return;
      } finally {
        bitmap.close();
      }
    } catch (error) {
      console.warn(`createImageBitmap failed, fallback to drawImage(VideoFrame): ${error.message}`);
    }
  }

  context.drawImage(frame, 0, 0, width, height);
}

function withTimeout(promise, timeoutMs, label) {
  if (timeoutMs <= 0) {
    return promise;
  }

  return Promise.race([
    promise,
    new Promise((_, reject) => {
      setTimeout(() => {
        reject(new Error(`${label} timed out after ${timeoutMs}ms`));
      }, timeoutMs);
    }),
  ]);
}

async function probeFrame(frame) {
  if (probeFramesRemaining <= 0 || typeof frame.allocationSize !== 'function' || typeof frame.copyTo !== 'function') {
    return;
  }

  probeFramesRemaining -= 1;

  try {
    const size = frame.allocationSize();
    const data = new Uint8Array(size);
    await frame.copyTo(data);

    let sum = 0;
    let nonZero = 0;
    const sampleLength = Math.min(data.length, 256);
    for (let i = 0; i < sampleLength; ++i) {
      const value = data[i];
      sum += value;
      if (value !== 0) {
        nonZero += 1;
      }
    }

    const message = `fmt=${frame.format || 'unknown'} sample=${sampleLength} nonZero=${nonZero} avg=${(sum / sampleLength).toFixed(1)}`;
    probeValue.textContent = message;
    console.log(`Shared texture probe: ${message}`);
  } catch (error) {
    probeValue.textContent = `probe failed: ${error.message}`;
    console.error('Shared texture probe failed:', error);
  }
}

function releasePacket(packet) {
  if (!packet) {
    return;
  }

  try {
    packet.importedSharedTexture.release();
  } catch {
    // Ignore release races during shutdown.
  }
}

async function drainFrameQueue() {
  if (drawInFlight) {
    return;
  }

  drawInFlight = true;
  while (pendingPacket) {
    const packet = pendingPacket;
    pendingPacket = null;

    const { importedSharedTexture, info } = packet;
    let frame;
    try {
      frame = importedSharedTexture.getVideoFrame();
      resizeCanvas(info.width, info.height);
      await withTimeout(
        drawFrame(frame, info.width, info.height),
        renderTimeoutMs,
        'drawFrame',
      );

      if (probeFramesRemaining > 0 && typeof frame.clone === 'function') {
        const probeFrameCopy = frame.clone();
        void probeFrame(probeFrameCopy).finally(() => {
          probeFrameCopy.close();
        });
      }

      statusBadge.textContent = 'Texture received';
      stateValue.textContent = 'receiving';
      sizeValue.textContent = `${info.width} x ${info.height}`;
      frameValue.textContent = String(info.frameId ?? '-');
      timestampValue.textContent = `${info.timestampUs ?? '-'} us`;
      if (textureValue) {
        textureValue.textContent = importedSharedTexture.textureId;
      }
      if (probeValue.textContent === '-') {
        probeValue.textContent = enableProbe ? `fmt=${frame.format || 'unknown'}` : 'disabled';
      }
    } catch (error) {
      console.error('Shared texture render failed:', error);
      statusBadge.textContent = 'Render failed';
      stateValue.textContent = error.message;
    } finally {
      if (frame) {
        frame.close();
      }
      releasePacket(packet);
    }
  }

  drawInFlight = false;
}

sharedTexture.setSharedTextureReceiver(({ importedSharedTexture }, info) => {
  if (pendingPacket) {
    // Keep renderer real-time: only keep the latest waiting frame.
    releasePacket(pendingPacket);
  }

  pendingPacket = { importedSharedTexture, info };
  void drainFrameQueue();
});

const onControlStatus = (_event, status) => {
  applyControlStatus(status);
};
ipcRenderer.on('uxplay-control:status', onControlStatus);

pinForm.addEventListener('submit', (event) => {
  void submitPinUpdate(event);
});
muteToggleButton.addEventListener('click', () => {
  void submitMuteToggle();
});
void initializePinControl();

window.addEventListener('beforeunload', () => {
  ipcRenderer.removeListener('uxplay-control:status', onControlStatus);
  releasePacket(pendingPacket);
  pendingPacket = null;
});
