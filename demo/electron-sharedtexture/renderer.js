const { ipcRenderer, sharedTexture } = require('electron');

if (!sharedTexture) {
  throw new Error('sharedTexture is unavailable in this renderer.');
}
if (!ipcRenderer) {
  throw new Error('ipcRenderer is unavailable in this renderer.');
}

const canvas = document.getElementById('videoCanvas');
const statusBadge = document.getElementById('statusBadge');
const toolbar = document.querySelector('.toolbar');
const pinSubmitButton = document.getElementById('pinSubmitButton');
const pinFeedback = document.getElementById('pinFeedback');
const muteToggleButton = document.getElementById('muteToggleButton');
const muteFeedback = document.getElementById('muteFeedback');
const stopCastButton = document.getElementById('stopCastButton');
const stopFeedback = document.getElementById('stopFeedback');
const fullscreenToggleButton = document.getElementById('fullscreenToggleButton');
const windowFeedback = document.getElementById('windowFeedback');

const context = canvas.getContext('2d', { alpha: false });
if (
  !context ||
  !statusBadge ||
  !toolbar ||
  !pinSubmitButton ||
  !pinFeedback ||
  !muteToggleButton ||
  !muteFeedback ||
  !stopCastButton ||
  !stopFeedback ||
  !fullscreenToggleButton ||
  !windowFeedback
) {
  throw new Error('Required UI elements are missing.');
}

const query =
  typeof window !== 'undefined' ? new URLSearchParams(window.location.search) : new URLSearchParams();
const renderTimeoutMs = Number.parseInt(query.get('renderTimeoutMs') || '250', 10) || 0;
const fitMode = (query.get('fit') || 'contain').toLowerCase();
const nativeEnv = typeof process !== 'undefined' && process?.env ? process.env : {};
const renderDprCap = parsePositiveNumber(
  query.get('dprCap') || nativeEnv.UXPLAY_RENDER_DPR_CAP,
  2,
);
const useImageBitmap = parseBoolean(
  query.get('imageBitmap') || nativeEnv.UXPLAY_RENDER_USE_IMAGE_BITMAP,
  true,
);
const cacheLastFrame = parseBoolean(
  query.get('cacheLastFrame') || nativeEnv.UXPLAY_RENDER_CACHE_LAST_FRAME,
  true,
);

let drawInFlight = false;
let drainScheduled = false;
let pendingPacket = null;
let controlSessionToken = null;
let currentMuted = null;
let windowFullscreen = null;
let windowControlsEnabled = false;
let castingActive = false;
let lastStatusText = '';
let lastFrameStatusAt = 0;
const lastFrameCanvas = document.createElement('canvas');
const lastFrameContext = lastFrameCanvas.getContext('2d', { alpha: false });
let lastFrameWidth = 0;
let lastFrameHeight = 0;

if (!lastFrameContext) {
  throw new Error('Unable to initialize cached frame context.');
}

function parseBoolean(value, fallback) {
  if (typeof value !== 'string') {
    return fallback;
  }

  const normalized = value.trim().toLowerCase();
  if (['1', 'true', 'yes', 'on'].includes(normalized)) {
    return true;
  }
  if (['0', 'false', 'no', 'off'].includes(normalized)) {
    return false;
  }
  return fallback;
}

function parsePositiveNumber(value, fallback) {
  const parsed = Number.parseFloat(value);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function setStatusText(text) {
  if (lastStatusText === text) {
    return;
  }
  lastStatusText = text;
  statusBadge.textContent = text;
}

function setFeedback(element, message, tone = 'muted') {
  element.textContent = message;
  element.dataset.tone = tone;
}

function updateMuteDisplay(muted) {
  currentMuted = typeof muted === 'boolean' ? muted : null;
  muteToggleButton.classList.toggle('is-muted', currentMuted === true);
  muteToggleButton.classList.toggle('is-active', currentMuted === true);
  toolbar.classList.toggle('is-active', currentMuted === true);

  if (currentMuted === true) {
    muteToggleButton.setAttribute('aria-label', '开启音频');
    return;
  }
  if (currentMuted === false) {
    muteToggleButton.setAttribute('aria-label', '关闭音频');
    return;
  }
  muteToggleButton.setAttribute('aria-label', '音频开关');
}

function updateWindowButtons() {
  const label = windowFullscreen ? '退出全屏' : '进入全屏';
  fullscreenToggleButton.setAttribute('aria-label', label);
  fullscreenToggleButton.disabled = !windowControlsEnabled;
}

function setWindowDisplayState(isFullscreen) {
  if (typeof isFullscreen !== 'boolean') {
    return;
  }
  windowFullscreen = isFullscreen;
  updateWindowButtons();
}

function setPinControlEnabled(enabled) {
  pinSubmitButton.disabled = !enabled;
}

function setMuteControlEnabled(enabled) {
  muteToggleButton.disabled = !enabled;
}

function setStopControlEnabled(enabled) {
  stopCastButton.disabled = !enabled;
}

function setWindowControlsEnabled(enabled) {
  windowControlsEnabled = enabled;
  updateWindowButtons();
}

function clearCanvasToBlack() {
  resizeCanvasToViewport();
  context.fillStyle = '#000000';
  context.fillRect(0, 0, canvas.width, canvas.height);
}

function drawSourceToViewport(source, width, height) {
  resizeCanvasToViewport();

  const canvasWidth = canvas.width;
  const canvasHeight = canvas.height;
  const scaleBase = fitMode === 'cover'
    ? Math.max(canvasWidth / width, canvasHeight / height)
    : Math.min(canvasWidth / width, canvasHeight / height);
  const scale = Number.isFinite(scaleBase) && scaleBase > 0 ? scaleBase : 1;
  const drawWidth = width * scale;
  const drawHeight = height * scale;
  const offsetX = (canvasWidth - drawWidth) * 0.5;
  const offsetY = (canvasHeight - drawHeight) * 0.5;

  context.fillStyle = '#000000';
  context.fillRect(0, 0, canvasWidth, canvasHeight);
  context.drawImage(source, offsetX, offsetY, drawWidth, drawHeight);
}

function drawFrameSource(source, width, height) {
  if (!cacheLastFrame) {
    drawSourceToViewport(source, width, height);
    return;
  }

  if (lastFrameCanvas.width !== width || lastFrameCanvas.height !== height) {
    lastFrameCanvas.width = width;
    lastFrameCanvas.height = height;
  }

  lastFrameContext.drawImage(source, 0, 0, width, height);
  lastFrameWidth = width;
  lastFrameHeight = height;
  drawSourceToViewport(lastFrameCanvas, width, height);
}

function redrawFromCachedFrame() {
  if (!cacheLastFrame) {
    return false;
  }

  if (lastFrameWidth <= 0 || lastFrameHeight <= 0) {
    return false;
  }

  drawSourceToViewport(lastFrameCanvas, lastFrameWidth, lastFrameHeight);
  return true;
}

function setCastingState(active) {
  const nextState = active === true;
  if (castingActive === nextState) {
    return;
  }
  castingActive = nextState;

  if (!castingActive) {
    if (pendingPacket) {
      releasePacket(pendingPacket);
      pendingPacket = null;
    }
    lastFrameWidth = 0;
    lastFrameHeight = 0;
    clearCanvasToBlack();
    setStatusText('未投屏');
    return;
  }

  setStatusText('投屏中（等待画面）');
}

function applyControlStatus(status) {
  const port = Number.isInteger(status?.port) ? status.port : null;
  const pin = typeof status?.pin === 'string' ? status.pin : null;
  const rotating = status?.rotating === true;
  const audioUpdating = status?.audioUpdating === true;
  const stopUpdating = status?.stopUpdating === true;
  const controlCastingActive = status?.castingActive === true;
  const muted = typeof status?.muted === 'boolean'
    ? status.muted
    : (typeof status?.mirrorAudioEnabled === 'boolean' ? !status.mirrorAudioEnabled : null);
  const hasPort = Boolean(port && port > 0);

  if (typeof status?.isFullscreen === 'boolean') {
    setWindowDisplayState(status.isFullscreen);
  }

  setCastingState(controlCastingActive);
  updateMuteDisplay(muted);
  setPinControlEnabled(hasPort && !rotating);
  setMuteControlEnabled(hasPort && !audioUpdating);
  setStopControlEnabled(hasPort && !stopUpdating);

  if (!hasPort) {
    setFeedback(pinFeedback, 'UxPlay 已启动，等待控制端口...', 'muted');
  } else if (rotating) {
    setFeedback(pinFeedback, '正在刷新 PIN...', 'muted');
  } else if (pin) {
    setFeedback(pinFeedback, `当前 PIN: ${pin}`, 'success');
  } else {
    setFeedback(pinFeedback, '等待首个投屏会话生成 PIN...', 'muted');
  }

  if (!hasPort) {
    setFeedback(muteFeedback, 'UxPlay 已启动，等待控制端口...', 'muted');
  } else if (audioUpdating) {
    setFeedback(muteFeedback, '正在应用音频设置...', 'muted');
  } else if (typeof muted === 'boolean') {
    setFeedback(muteFeedback, `当前音频${muted ? '已静音' : '未静音'}`, 'muted');
  } else {
    setFeedback(muteFeedback, '等待 /api/audio 返回状态...', 'muted');
  }

  if (!hasPort) {
    setFeedback(stopFeedback, 'UxPlay 已启动，等待控制端口...', 'muted');
  } else if (stopUpdating) {
    setFeedback(stopFeedback, '正在停止当前投屏...', 'muted');
  } else {
    setFeedback(stopFeedback, '可停止当前投屏会话。', 'muted');
  }
}

async function refreshControlSession() {
  try {
    const session = await ipcRenderer.invoke('uxplay-control:get-session');
    if (session?.ok) {
      controlSessionToken = session.token;
      setWindowControlsEnabled(true);
      applyControlStatus(session);
    }
    return session;
  } catch {
    return null;
  }
}

async function initializeControls() {
  setPinControlEnabled(false);
  setMuteControlEnabled(false);
  setStopControlEnabled(false);
  setWindowControlsEnabled(false);
  updateMuteDisplay(null);
  setCastingState(false);

  try {
    const [session, windowState] = await Promise.all([
      ipcRenderer.invoke('uxplay-control:get-session'),
      ipcRenderer.invoke('window-control:get-state'),
    ]);

    if (!session?.ok) {
      const message = session?.error?.message || 'unknown error';
      setFeedback(pinFeedback, `PIN 控制不可用: ${message}`, 'error');
      setFeedback(muteFeedback, `音频控制不可用: ${message}`, 'error');
      setFeedback(stopFeedback, `停止控制不可用: ${message}`, 'error');
    } else {
      controlSessionToken = session.token;
      setWindowControlsEnabled(true);
      applyControlStatus(session);
    }

    if (!windowState?.ok) {
      setFeedback(windowFeedback, `窗口状态同步失败: ${windowState?.error?.message || 'unknown error'}`, 'error');
    } else {
      setWindowDisplayState(windowState.isFullscreen === true);
      setFeedback(windowFeedback, windowState.isFullscreen ? '当前为全屏模式' : '当前为窗口模式', 'muted');
    }
  } catch (error) {
    setFeedback(pinFeedback, `PIN 控制初始化失败: ${error.message}`, 'error');
    setFeedback(muteFeedback, `音频控制初始化失败: ${error.message}`, 'error');
    setFeedback(stopFeedback, `停止控制初始化失败: ${error.message}`, 'error');
    setFeedback(windowFeedback, `窗口状态初始化失败: ${error.message}`, 'error');
  }
}

async function submitPinUpdate() {
  await refreshControlSession();

  if (!controlSessionToken) {
    setFeedback(pinFeedback, '控制会话未初始化。', 'error');
    return;
  }

  setPinControlEnabled(false);
  setFeedback(pinFeedback, '正在刷新 PIN...', 'muted');

  try {
    const response = await ipcRenderer.invoke('uxplay-control:rotate-pin', {
      token: controlSessionToken,
    });

    if (!response?.ok) {
      setFeedback(
        pinFeedback,
        `PIN 刷新失败 [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    applyControlStatus(response);
    setFeedback(
      pinFeedback,
      `PIN 已更新为 ${response?.pin}。${response?.result?.message || '已应用。'}`,
      'success',
    );
  } catch (error) {
    setFeedback(pinFeedback, `PIN 刷新请求失败: ${error.message}`, 'error');
  } finally {
    await refreshControlSession();
  }
}

async function submitMuteToggle() {
  await refreshControlSession();

  if (!controlSessionToken) {
    setFeedback(muteFeedback, '控制会话未初始化。', 'error');
    return;
  }

  const targetMuted = currentMuted === null ? true : !currentMuted;
  setMuteControlEnabled(false);
  setFeedback(muteFeedback, `${targetMuted ? '正在静音' : '正在取消静音'}...`, 'muted');

  try {
    const response = await ipcRenderer.invoke('uxplay-control:set-muted', {
      token: controlSessionToken,
      muted: targetMuted,
    });

    if (!response?.ok) {
      setFeedback(
        muteFeedback,
        `音频切换失败 [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    updateMuteDisplay(response?.muted);
    setFeedback(
      muteFeedback,
      `${response?.muted ? '已静音' : '已恢复音频'}。${response?.result?.message || '已应用。'}`,
      'success',
    );
  } catch (error) {
    setFeedback(muteFeedback, `音频切换请求失败: ${error.message}`, 'error');
  } finally {
    await refreshControlSession();
  }
}

async function submitStopCasting() {
  await refreshControlSession();

  if (!controlSessionToken) {
    setFeedback(stopFeedback, '控制会话未初始化。', 'error');
    return;
  }

  setStopControlEnabled(false);
  setFeedback(stopFeedback, '正在停止当前投屏...', 'muted');

  try {
    const response = await ipcRenderer.invoke('uxplay-control:stop-casting', {
      token: controlSessionToken,
    });

    if (!response?.ok) {
      setFeedback(
        stopFeedback,
        `停止投屏失败 [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    applyControlStatus(response);
    setFeedback(
      stopFeedback,
      `投屏已停止。${response?.result?.message || '已应用。'}`,
      'success',
    );
  } catch (error) {
    setFeedback(stopFeedback, `停止投屏请求失败: ${error.message}`, 'error');
  } finally {
    await refreshControlSession();
  }
}

async function toggleFullscreen() {
  await refreshControlSession();

  if (!controlSessionToken) {
    setFeedback(windowFeedback, '控制会话未初始化。', 'error');
    return;
  }

  setWindowControlsEnabled(false);
  setFeedback(windowFeedback, '正在切换全屏状态...', 'muted');

  try {
    const response = await ipcRenderer.invoke('window-control:toggle-fullscreen', {
      token: controlSessionToken,
    });

    if (!response?.ok) {
      setFeedback(
        windowFeedback,
        `全屏切换失败 [${response?.error?.code || 'UNKNOWN'}]: ${response?.error?.message || 'unknown error'}`,
        'error',
      );
      return;
    }

    setWindowDisplayState(response.isFullscreen === true);
    setFeedback(windowFeedback, response.isFullscreen ? '已进入全屏模式。' : '已切换为窗口模式。', 'success');
  } catch (error) {
    setFeedback(windowFeedback, `全屏切换请求失败: ${error.message}`, 'error');
  } finally {
    setWindowControlsEnabled(true);
  }
}

function resizeCanvasToViewport() {
  const nativeDpr = window.devicePixelRatio || 1;
  const dpr = Math.max(1, Math.min(nativeDpr, renderDprCap));
  const targetWidth = Math.max(1, Math.round(window.innerWidth * dpr));
  const targetHeight = Math.max(1, Math.round(window.innerHeight * dpr));

  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    canvas.width = targetWidth;
    canvas.height = targetHeight;
  }
}

async function drawFrame(frame, width, height) {
  if (useImageBitmap && typeof createImageBitmap === 'function') {
    try {
      const bitmap = await createImageBitmap(frame);
      try {
        drawFrameSource(bitmap, width, height);
        return;
      } finally {
        bitmap.close();
      }
    } catch (error) {
      console.warn(`createImageBitmap failed, fallback to drawImage(VideoFrame): ${error.message}`);
    }
  }

  drawFrameSource(frame, width, height);
}

function withTimeout(promise, timeoutMs, label) {
  if (timeoutMs <= 0) {
    return promise;
  }

  let timeoutHandle;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timeoutHandle = setTimeout(() => {
        reject(new Error(`${label} timed out after ${timeoutMs}ms`));
      }, timeoutMs);
    }),
  ]).finally(() => {
    if (timeoutHandle) {
      clearTimeout(timeoutHandle);
    }
  });
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

  if (!pendingPacket) {
    return;
  }

  drawInFlight = true;
  const packet = pendingPacket;
  pendingPacket = null;

  if (!castingActive) {
    releasePacket(packet);
    drawInFlight = false;
    return;
  }

  const { importedSharedTexture, info } = packet;
  let frame;
  try {
    frame = importedSharedTexture.getVideoFrame();
    await withTimeout(
      drawFrame(frame, info.width, info.height),
      renderTimeoutMs,
      'drawFrame',
    );

    if (castingActive) {
      const now = performance.now();
      if (now - lastFrameStatusAt > 500) {
        lastFrameStatusAt = now;
        setStatusText(`投屏中 ${info.width}x${info.height}`);
      }
    } else {
      clearCanvasToBlack();
      setStatusText('未投屏');
    }
  } catch (error) {
    console.error('Shared texture render failed:', error);
    setStatusText(castingActive ? `渲染失败: ${error.message}` : '未投屏');
  } finally {
    if (frame) {
      frame.close();
    }
    releasePacket(packet);
    drawInFlight = false;
  }

  if (pendingPacket) {
    scheduleDrainFrameQueue();
  }
}

function scheduleDrainFrameQueue() {
  if (drainScheduled) {
    return;
  }

  drainScheduled = true;
  requestAnimationFrame(() => {
    drainScheduled = false;
    void drainFrameQueue();
  });
}

sharedTexture.setSharedTextureReceiver(({ importedSharedTexture }, info) => {
  if (!castingActive) {
    releasePacket({ importedSharedTexture });
    return;
  }

  if (pendingPacket) {
    // Keep renderer real-time: only keep the latest waiting frame.
    releasePacket(pendingPacket);
  }

  pendingPacket = { importedSharedTexture, info };
  scheduleDrainFrameQueue();
});

const onControlStatus = (_event, status) => {
  applyControlStatus(status);
};
ipcRenderer.on('uxplay-control:status', onControlStatus);

pinSubmitButton.addEventListener('click', () => {
  void submitPinUpdate();
});
muteToggleButton.addEventListener('click', () => {
  void submitMuteToggle();
});
stopCastButton.addEventListener('click', () => {
  void submitStopCasting();
});
fullscreenToggleButton.addEventListener('click', () => {
  void toggleFullscreen();
});

window.addEventListener('resize', () => {
  resizeCanvasToViewport();
  if (!castingActive || !redrawFromCachedFrame()) {
    clearCanvasToBlack();
  }
});

resizeCanvasToViewport();
clearCanvasToBlack();
void initializeControls();

window.addEventListener('beforeunload', () => {
  ipcRenderer.removeListener('uxplay-control:status', onControlStatus);
  releasePacket(pendingPacket);
  pendingPacket = null;
});
