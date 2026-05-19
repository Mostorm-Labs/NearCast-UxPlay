const { app, BrowserWindow, Menu, Tray, ipcMain, nativeImage, safeStorage, screen, sharedTexture } = require('electron');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const WebSocket = require('ws');

// 启用调试模式
if (process.env.NODE_ENV === 'development') {
  app.commandLine.appendSwitch('remote-debugging-port', '9222');
  app.commandLine.appendSwitch('inspect', '5858');
}

const repoRoot = path.resolve(__dirname, '..', '..');
const packagedUxplayRuntimeDir = path.join(process.resourcesPath, 'uxplay-runtime');
const defaultUxplayExecutable = app.isPackaged
  ? path.join(packagedUxplayRuntimeDir, 'uxplay.exe')
  : path.join(repoRoot, 'build', 'uxplay.exe');
const uxplayExecutable =
  process.env.UXPLAY_EXE || defaultUxplayExecutable;
const uxplayWorkdir = path.dirname(uxplayExecutable);
const uxplayServerName =
  process.env.UXPLAY_SERVER_NAME || 'UxPlay SharedTexture';
const msysRoot = process.env.MSYS2_ROOT || 'D:\\msys64';
const gstreamerBin = app.isPackaged
  ? uxplayWorkdir
  : path.join(msysRoot, 'mingw64', 'bin');
const gstreamerPluginPath =
  process.env.GST_PLUGIN_PATH || path.join(uxplayWorkdir, 'lib', 'gstreamer-1.0');
const gstreamerPluginScanner = path.join(
  uxplayWorkdir,
  'libexec',
  'gstreamer-1.0',
  'gst-plugin-scanner.exe',
);
const traceSharedTexture =
  process.env.UXPLAY_TRACE_SHARED_TEXTURE === '1' ||
  process.env.UXPLAY_TRACE_SHARED_TEXTURE === 'true';
const sendTimeoutMs = Number.parseInt(process.env.UXPLAY_SEND_TIMEOUT_MS || '1200', 10) || 0;
const uxplayControlTimeoutMs = Number.parseInt(process.env.UXPLAY_CONTROL_TIMEOUT_MS || '3000', 10) || 3000;
const uxplayWsResponseLimitBytes = 16 * 1024;
const uxplayWsPort = Number.parseInt(process.env.UXPLAY_WS_PORT || '7001', 10) || 7001;
const uxplayWsEnabled = process.env.UXPLAY_WS_ENABLE !== '0' && process.env.UXPLAY_WS_ENABLE !== 'false';
const appWsEnabled = process.env.UXPLAY_APP_WS_ENABLE !== '0' && process.env.UXPLAY_APP_WS_ENABLE !== 'false';
const appWsAllowLan =
  process.env.UXPLAY_APP_WS_ALLOW_LAN === '1' ||
  process.env.UXPLAY_APP_WS_ALLOW_LAN === 'true';
const requestedAppWsHost = process.env.UXPLAY_APP_WS_HOST || '127.0.0.1';
const appWsHost = appWsAllowLan ? requestedAppWsHost : '127.0.0.1';
const appWsPort = Number.parseInt(process.env.UXPLAY_APP_WS_PORT || '7010', 10) || 7010;
const mirrorSessionIdleMs = Number.parseInt(process.env.UXPLAY_MIRROR_IDLE_MS || '3000', 10) || 3000;
const castWindowFullscreen =
  process.env.UXPLAY_CAST_FULLSCREEN !== '0' &&
  process.env.UXPLAY_CAST_FULLSCREEN !== 'false';
const pinWindowAutoHideMs = Number.parseInt(process.env.UXPLAY_PIN_WINDOW_AUTO_HIDE_MS || '6000', 10) || 6000;
const uxplayPinStoreFileName = 'uxplay-pin-state.json';
const controlSessionToken = crypto.randomBytes(24).toString('hex');
const appWsToken = process.env.UXPLAY_APP_WS_TOKEN || controlSessionToken;
const serverPortLogPattern = /Initialized server socket\(s\) on port (\d{1,5})/;

let win;
let pinWindow;
let tray;
let isExplicitlyQuitting = false;
let appReady = false;
let appWsServer;
const appWsClients = new Set();
let lastError = null;
let pinWindowAutoHideTimer = null;
let bridge;
let bridgeReader;
let bridgeReady = false;
let uxplayHttpPort = null;
let uxplayWsRequestSequence = 0;
let uxplayEventWs = null;
let uxplayEventAuthenticated = false;
let uxplayEventReconnectTimer = null;
let uxplayStderrBuffer = '';
let currentPin = null;
let pinUpdateInFlight = false;
let mirrorAudioEnabled = null;
let mirrorAudioUpdateInFlight = false;
let stopUpdateInFlight = false;
let castingActive = false;
let mirrorSessionActive = false;
let mirrorSessionIdleTimer = null;
let autoPinRotatedForCastingSession = false;
let frameSending = false;
let pendingFrame = null;
let framesReceived = 0;
let framesQueued = 0;
let framesSent = 0;
let frameReleases = 0;
let importFailures = 0;
let sendFailures = 0;
let sendTimeouts = 0;

if (!app || !BrowserWindow || !sharedTexture) {
  throw new Error('Electron sharedTexture API is unavailable. Use Electron 40+ and ensure ELECTRON_RUN_AS_NODE is not set.');
}

const gotSingleInstanceLock = app.requestSingleInstanceLock();
if (!gotSingleInstanceLock) {
  app.quit();
}

function splitCommandLineArgs(text) {
  if (!text) {
    return [];
  }

  const parts = text.match(/"[^"]*"|'[^']*'|[^\s]+/g) || [];
  return parts.map((part) => {
    if (
      (part.startsWith('"') && part.endsWith('"')) ||
      (part.startsWith("'") && part.endsWith("'"))
    ) {
      return part.slice(1, -1);
    }

    return part;
  });
}

function getWsUrl(host, port) {
  if (!host || !Number.isInteger(port) || port <= 0 || port > 65535) {
    return null;
  }
  return `ws://${host}:${port}/`;
}

function rememberLastError(error, fallbackCode = 'INTERNAL_ERROR') {
  const formatted = formatControlError(error || createControlError(fallbackCode, 'unknown error'));
  lastError = {
    ...formatted,
    at: new Date().toISOString(),
  };
  return lastError;
}

function getPinWindowState() {
  return {
    available: Boolean(pinWindow && !pinWindow.isDestroyed()),
    visible: Boolean(pinWindow && !pinWindow.isDestroyed() && pinWindow.isVisible()),
  };
}

function getFrameStats() {
  return {
    received: framesReceived,
    queued: framesQueued,
    sent: framesSent,
    releases: frameReleases,
    importFailures,
    sendFailures,
    sendTimeouts,
    pending: Boolean(pendingFrame),
    sending: frameSending,
  };
}

function getAppStatus() {
  const castWindowState = getCastWindowState();
  const pinWindowState = getPinWindowState();
  const uxplayWsUrl = uxplayWsEnabled ? getWsUrl('127.0.0.1', uxplayWsPort) : null;
  const electronWsUrl = appWsEnabled ? getWsUrl(appWsHost, appWsPort) : null;
  const muted = typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null;

  return {
    app: {
      ready: appReady,
      packaged: app.isPackaged,
      name: uxplayServerName,
    },
    uxplay: {
      ready: bridgeReady,
      pid: bridge?.pid || null,
      httpPort: uxplayHttpPort,
      ws: {
        enabled: uxplayWsEnabled,
        host: uxplayWsEnabled ? '127.0.0.1' : null,
        port: uxplayWsEnabled ? uxplayWsPort : null,
        url: uxplayWsUrl,
      },
    },
    electronWs: {
      enabled: appWsEnabled,
      host: appWsEnabled ? appWsHost : null,
      port: appWsEnabled ? appWsPort : null,
      url: electronWsUrl,
      allowLan: appWsAllowLan,
      connectedClients: appWsClients.size,
      authenticatedClients: [...appWsClients].filter((client) => client.isAuthenticated).length,
    },
    casting: {
      active: castingActive,
      mirrorSessionActive,
      frameStats: getFrameStats(),
    },
    pinState: {
      value: currentPin,
      updating: pinUpdateInFlight,
      window: pinWindowState,
    },
    audio: {
      mirrorAudioEnabled,
      muted,
      updating: mirrorAudioUpdateInFlight,
    },
    windows: {
      cast: castWindowState,
      pin: pinWindowState,
    },
    control: {
      stopUpdating: stopUpdateInFlight,
    },
    error: lastError,

    // Backward-compatible flat shape used by the current renderer.
    ready: bridgeReady,
    port: uxplayHttpPort,
    wsPort: uxplayWsEnabled ? uxplayWsPort : null,
    wsUrl: uxplayWsUrl,
    appWsPort: appWsEnabled ? appWsPort : null,
    appWsUrl: electronWsUrl,
    castingActive,
    pin: currentPin,
    rotating: pinUpdateInFlight,
    mirrorAudioEnabled,
    muted,
    audioUpdating: mirrorAudioUpdateInFlight,
    stopUpdating: stopUpdateInFlight,
    isFullscreen: castWindowState.fullscreen,
    windowVisible: castWindowState.visible,
    castWindowVisible: castWindowState.visible,
    pinWindowVisible: pinWindowState.visible,
  };
}

function sendJsonToWebSocket(ws, payload) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    return false;
  }
  ws.send(JSON.stringify(payload));
  return true;
}

function createAppWsSessionId() {
  return crypto.randomBytes(8).toString('hex');
}

function ensureAppWsSessionId(ws, sidHint = null) {
  if (typeof sidHint === 'string' && sidHint.trim()) {
    ws.sessionId = sidHint.trim().slice(0, 30);
  }
  if (!ws.sessionId) {
    ws.sessionId = createAppWsSessionId();
  }
  return ws.sessionId;
}

function isAppWsAdvanced(ws) {
  return ws?.protocolMode === 'advanced';
}

function makeAppWsEventPayload(ws, op, data = {}) {
  if (isAppWsAdvanced(ws)) {
    return {
      sid: ensureAppWsSessionId(ws),
      op: 6,
      d: {
        event: op,
        data,
      },
    };
  }
  return {
    type: 'event',
    op,
    data,
  };
}

function broadcastAppEvent(op, data = {}) {
  for (const client of appWsClients) {
    if (client.isAuthenticated) {
      sendJsonToWebSocket(client, makeAppWsEventPayload(client, op, data));
    }
  }
}

function broadcastControlStatus() {
  const status = getAppStatus();
  if (win && !win.isDestroyed()) {
    win.webContents.send('uxplay-control:status', status);
  }
  if (pinWindow && !pinWindow.isDestroyed()) {
    pinWindow.webContents.send('uxplay-control:status', status);
  }
  broadcastAppEvent('status.changed', status);
}

function createControlError(code, message, extras = {}) {
  const error = new Error(message);
  error.code = code;
  Object.assign(error, extras);
  return error;
}

function getPinStorePath() {
  return path.join(app.getPath('userData'), uxplayPinStoreFileName);
}

function formatControlError(error) {
  return {
    code: error?.code || 'INTERNAL_ERROR',
    message: error?.message || 'unknown error',
    httpStatus: Number.isInteger(error?.httpStatus) ? error.httpStatus : 500,
    details: error?.details && typeof error.details === 'object' ? error.details : undefined,
  };
}

function updateHttpPortFromLogLine(line) {
  if (!line) {
    return;
  }
  const match = serverPortLogPattern.exec(line);
  if (!match) {
    return;
  }
  const parsedPort = Number.parseInt(match[1], 10);
  if (Number.isInteger(parsedPort) && parsedPort > 0 && parsedPort <= 65535) {
    const changed = uxplayHttpPort !== parsedPort;
    uxplayHttpPort = parsedPort;
    if (changed) {
      broadcastAppEvent('control.portChanged', {
        port: uxplayHttpPort,
      });
      broadcastControlStatus();
      scheduleUxplayEventWebSocketReconnect('port-ready', 0);
      void triggerAutoPinRotationOnPortReady();
      void refreshMirrorAudioStateFromUxPlay('port-ready');
    }
  }
}

function consumeUxplayStderrChunk(chunk) {
  const text = chunk.toString('utf8');
  uxplayStderrBuffer += text;
  const lines = uxplayStderrBuffer.split(/\r?\n/);
  uxplayStderrBuffer = lines.pop() || '';
  for (const line of lines) {
    updateHttpPortFromLogLine(line);
  }
}

function normalizePinInput(rawPin) {
  const pin = typeof rawPin === 'number' ? String(rawPin) : String(rawPin || '').trim();
  if (!/^\d{4}$/.test(pin)) {
    throw createControlError(
      'INVALID_PIN',
      'PIN must be exactly 4 numeric digits.',
      { httpStatus: 400 },
    );
  }
  return pin;
}

function normalizeOptionalPin(rawPin) {
  if (typeof rawPin === 'undefined' || rawPin === null) {
    return null;
  }
  const pin = typeof rawPin === 'number' ? String(rawPin).padStart(4, '0') : String(rawPin).trim();
  return /^\d{4}$/.test(pin) ? pin : null;
}

function extractMirrorAudioEnabled(responseBody) {
  if (responseBody && typeof responseBody === 'object' && typeof responseBody.mirrorAudio === 'boolean') {
    return responseBody.mirrorAudio;
  }
  return null;
}

function normalizeMutedInput(rawMuted) {
  if (typeof rawMuted === 'boolean') {
    return rawMuted;
  }
  if (typeof rawMuted === 'string') {
    const normalized = rawMuted.trim().toLowerCase();
    if (normalized === '1' || normalized === 'true' || normalized === 'yes' || normalized === 'on') {
      return true;
    }
    if (normalized === '0' || normalized === 'false' || normalized === 'no' || normalized === 'off') {
      return false;
    }
  }
  throw createControlError('INVALID_MUTED_VALUE', 'Muted must be a boolean value.', {
    httpStatus: 400,
  });
}

function generateRandomPin(excludePin = null) {
  for (let attempt = 0; attempt < 6; ++attempt) {
    const candidate = String(crypto.randomInt(0, 10000)).padStart(4, '0');
    if (!excludePin || candidate !== excludePin) {
      return candidate;
    }
  }
  return String(crypto.randomInt(0, 10000)).padStart(4, '0');
}

function ensureUxplayWebSocketReady() {
  if (!uxplayWsEnabled) {
    throw createControlError('UXPLAY_WS_DISABLED', 'UxPlay WebSocket control is disabled.', {
      httpStatus: 503,
    });
  }
  if (!Number.isInteger(uxplayWsPort) || uxplayWsPort <= 0 || uxplayWsPort > 65535) {
    throw createControlError('UXPLAY_WS_PORT_UNAVAILABLE', 'UxPlay WebSocket control port is not configured.', {
      httpStatus: 503,
    });
  }
  if (!bridge) {
    throw createControlError('UXPLAY_UNAVAILABLE', 'UxPlay process is not running.', {
      httpStatus: 503,
    });
  }
}

function shouldRetryAutoPinUpdate(error) {
  return (
    error?.code === 'UXPLAY_WS_DISABLED' ||
    error?.code === 'UXPLAY_WS_PORT_UNAVAILABLE' ||
    error?.code === 'UXPLAY_UNAVAILABLE' ||
    error?.code === 'UPSTREAM_TIMEOUT' ||
    error?.code === 'UPSTREAM_UNREACHABLE' ||
    error?.code === 'UPSTREAM_REJECTED'
  );
}

function touchMirrorSessionActivity() {
  if (mirrorSessionIdleTimer) {
    clearTimeout(mirrorSessionIdleTimer);
  }
  mirrorSessionIdleTimer = setTimeout(() => {
    mirrorSessionActive = false;
  }, mirrorSessionIdleMs);
}

function clearMirrorSessionIdleTimer() {
  if (!mirrorSessionIdleTimer) {
    return;
  }
  clearTimeout(mirrorSessionIdleTimer);
  mirrorSessionIdleTimer = null;
}

function clearPendingMainFrame(reason = 'unspecified') {
  if (!pendingFrame) {
    return;
  }

  const frameId = pendingFrame.frameId;
  releasePendingFrame(pendingFrame, { notifyRelease: true });
  pendingFrame = null;
  if (traceSharedTexture) {
    traceSharedTextureStats(`drop-${reason}-${frameId}`);
  }
}

function getCastWindowState() {
  return {
    visible: Boolean(win && !win.isDestroyed() && win.isVisible()),
    fullscreen: Boolean(win && !win.isDestroyed() && win.isFullScreen()),
  };
}

function updateTrayMenu() {
  if (!tray) {
    return;
  }

  const { visible } = getCastWindowState();
  const contextMenu = Menu.buildFromTemplate([
    {
      label: castingActive ? '投屏中' : '等待投屏',
      enabled: false,
    },
    {
      label: visible ? '隐藏投屏窗口' : '显示投屏窗口',
      click: () => {
        if (visible) {
          hideCastWindow('tray');
        } else {
          showCastWindow('tray');
        }
      },
    },
    {
      type: 'separator',
    },
    {
      label: '退出',
      click: () => {
        quitApplication();
      },
    },
  ]);
  tray.setContextMenu(contextMenu);
  tray.setToolTip(castingActive ? `${uxplayServerName} - 投屏中` : `${uxplayServerName} - 后台运行`);
}

function createTrayIcon() {
  const image = nativeImage.createFromDataURL(
    'data:image/png;base64,' +
      'iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAACjSURBVFhH7ZXNCYAwDEZdwZsbdBov3t3HgzdH8gecqvIhiJTPEG2Lggk8WohNXnOwRVk5/yYmYAImQAXqpvVdPyQHdcNeVAAf5wjUDXuJAuO8HvYxoA4C+7CXKMAOPEGqZwImYAIm8G2B13/FqUMtoH2Op2W/GVaWD1E/x1pQFIGV5TXcFjhPh02A3VLitgCaSIE8O3dF1AQY2SeQGhP4u4DzGwJ201+dn0XKAAAAAElFTkSuQmCC',
  );
  return image.isEmpty() ? nativeImage.createFromPath(process.execPath) : image;
}

function createTray() {
  if (tray) {
    return;
  }
  tray = new Tray(createTrayIcon());
  tray.on('double-click', () => {
    if (win && !win.isDestroyed() && win.isVisible()) {
      hideCastWindow('tray-double-click');
    } else {
      showCastWindow('tray-double-click');
    }
  });
  updateTrayMenu();
}

function showCastWindow(reason = 'unspecified') {
  if (!win || win.isDestroyed()) {
    return;
  }

  if (castWindowFullscreen !== win.isFullScreen()) {
    win.setFullScreen(castWindowFullscreen);
  }
  if (!win.isVisible()) {
    win.show();
  }
  broadcastAppEvent('window.changed', {
    window: 'cast',
    action: 'show',
    reason,
    state: getCastWindowState(),
  });
  updateTrayMenu();
  if (traceSharedTexture) {
    console.log(`[window] cast window shown reason=${reason}`);
  }
}

function hideCastWindow(reason = 'unspecified') {
  if (!win || win.isDestroyed()) {
    return;
  }

  if (win.isVisible()) {
    win.hide();
  }
  if (win.isFullScreen()) {
    win.setFullScreen(false);
  }
  broadcastAppEvent('window.changed', {
    window: 'cast',
    action: 'hide',
    reason,
    state: getCastWindowState(),
  });
  updateTrayMenu();
  if (traceSharedTexture) {
    console.log(`[window] cast window hidden reason=${reason}`);
  }
}

function clearPinWindowAutoHideTimer() {
  if (!pinWindowAutoHideTimer) {
    return;
  }
  clearTimeout(pinWindowAutoHideTimer);
  pinWindowAutoHideTimer = null;
}

function positionPinWindow(targetWindow) {
  if (!targetWindow || targetWindow.isDestroyed()) {
    return;
  }
  const display = screen.getPrimaryDisplay();
  const workArea = display.workArea || display.bounds;
  const [windowWidth, windowHeight] = targetWindow.getSize();
  targetWindow.setPosition(
    Math.round(workArea.x + (workArea.width - windowWidth) / 2),
    Math.round(workArea.y + 24),
    false,
  );
}

function schedulePinWindowAutoHide(reason = 'timeout') {
  clearPinWindowAutoHideTimer();
  if (!Number.isInteger(pinWindowAutoHideMs) || pinWindowAutoHideMs <= 0) {
    return;
  }
  pinWindowAutoHideTimer = setTimeout(() => {
    pinWindowAutoHideTimer = null;
    hidePinWindow(reason);
  }, pinWindowAutoHideMs);
}

function createPinWindow() {
  if (pinWindow && !pinWindow.isDestroyed()) {
    return pinWindow;
  }

  pinWindow = new BrowserWindow({
    width: 260,
    height: 70,
    minWidth: 260,
    minHeight: 70,
    resizable: false,
    maximizable: false,
    fullscreenable: false,
    frame: false,
    transparent: true,
    alwaysOnTop: true,
    autoHideMenuBar: true,
    focusable: false,
    show: false,
    skipTaskbar: true,
    hasShadow: false,
    backgroundColor: '#00000000',
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
    },
  });

  pinWindow.removeMenu();
  pinWindow.setMenuBarVisibility(false);
  pinWindow.setAlwaysOnTop(true, 'screen-saver');
  pinWindow.setIgnoreMouseEvents(true, { forward: true });
  pinWindow.loadFile(path.join(__dirname, 'pin.html'));
  pinWindow.webContents.once('did-finish-load', () => {
    pinWindow.webContents.send('uxplay-control:status', getAppStatus());
  });
  pinWindow.on('show', () => {
    broadcastAppEvent('window.changed', {
      window: 'pin',
      action: 'show',
      state: getPinWindowState(),
    });
    broadcastControlStatus();
  });
  pinWindow.on('hide', () => {
    broadcastAppEvent('window.changed', {
      window: 'pin',
      action: 'hide',
      state: getPinWindowState(),
    });
    broadcastAppEvent('pin.hidden', {
      pin: currentPin,
    });
    clearPinWindowAutoHideTimer();
    broadcastControlStatus();
  });
  pinWindow.on('close', (event) => {
    if (isExplicitlyQuitting) {
      return;
    }
    event.preventDefault();
    hidePinWindow('window-close');
  });
  pinWindow.on('closed', () => {
    clearPinWindowAutoHideTimer();
    pinWindow = null;
    broadcastControlStatus();
  });

  return pinWindow;
}

function showPinWindow(reason = 'unspecified') {
  const targetWindow = createPinWindow();
  if (!targetWindow || targetWindow.isDestroyed()) {
    return;
  }

  positionPinWindow(targetWindow);
  if (!targetWindow.isVisible()) {
    targetWindow.showInactive();
  }
  targetWindow.webContents.send('uxplay-control:status', getAppStatus());
  schedulePinWindowAutoHide('pin-auto-hide');
  broadcastAppEvent('pin.required', {
    pin: currentPin,
    reason,
  });
  broadcastControlStatus();
}

function hidePinWindow(reason = 'unspecified') {
  if (!pinWindow || pinWindow.isDestroyed()) {
    return;
  }
  clearPinWindowAutoHideTimer();
  if (!pinWindow.isVisible()) {
    return;
  }
  pinWindow.hide();
  broadcastControlStatus();
}

function quitApplication() {
  isExplicitlyQuitting = true;
  app.quit();
}

function setCastingActive(nextActive, reason = 'unspecified', options = {}) {
  const normalized = nextActive === true;
  if (!normalized || options.clearPending === true) {
    clearPendingMainFrame(`casting-${reason}`);
  }
  if (castingActive === normalized) {
    if (castingActive) {
      showCastWindow(reason);
      void flushFrameQueue();
    } else {
      hideCastWindow(reason);
    }
    return;
  }
  castingActive = normalized;
  if (castingActive) {
    mirrorSessionActive = true;
    autoPinRotatedForCastingSession = false;
    if (pinWindow && !pinWindow.isDestroyed() && pinWindow.isVisible()) {
      broadcastAppEvent('pin.accepted', {
        pin: currentPin,
        reason,
      });
      hidePinWindow('casting-started');
    }
    showCastWindow(reason);
    broadcastAppEvent('casting.started', {
      reason,
      status: getAppStatus(),
    });
    broadcastAppEvent('mirrorStarted', {
      reason,
    });
    void triggerAutoPinRotationOnMirrorStart();
  } else {
    mirrorSessionActive = false;
    autoPinRotatedForCastingSession = false;
    clearMirrorSessionIdleTimer();
    hideCastWindow(reason);
    broadcastAppEvent('casting.stopped', {
      reason,
      status: getAppStatus(),
    });
    broadcastAppEvent('mirrorStopped', {
      reason,
    });
  }
  if (traceSharedTexture) {
    console.log(`[shared-texture:session] castingActive=${castingActive} reason=${reason}`);
  }
  updateTrayMenu();
  broadcastControlStatus();
  if (castingActive) {
    void flushFrameQueue();
  }
}

function ensureTrustedSender(event) {
  if (!win || win.isDestroyed() || !event?.sender || !event?.senderFrame) {
    throw createControlError('FORBIDDEN', 'Unauthorized caller.', { httpStatus: 403 });
  }
  const currentWindowUrl = win.webContents.getURL();
  const senderUrl = event.senderFrame.url || '';
  if (event.sender.id !== win.webContents.id || !senderUrl.startsWith('file://') || senderUrl !== currentWindowUrl) {
    throw createControlError('FORBIDDEN', 'Caller does not have pin control permission.', { httpStatus: 403 });
  }
}

function ensureControlToken(token) {
  if (typeof token !== 'string' || token.length < 16 || token !== controlSessionToken) {
    throw createControlError('FORBIDDEN', 'Invalid control token.', { httpStatus: 403 });
  }
}

function delay(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

function nextUxplayWsRequestId(op) {
  uxplayWsRequestSequence += 1;
  return `uxplay-${process.pid}-${Date.now()}-${uxplayWsRequestSequence}-${op}`;
}

function makeUxplayWsRejectError(response, op) {
  const upstreamError = response?.error || 'UPSTREAM_REJECTED';
  const message =
    response?.data?.message ||
    response?.data?.error?.message ||
    `UxPlay WebSocket ${op} request failed: ${upstreamError}.`;
  return createControlError('UPSTREAM_REJECTED', message, {
    httpStatus: 502,
    details: {
      upstreamError,
      op,
    },
  });
}

function requestUxplayWebSocketOnce(op, data = {}, fallbackMessage = 'ok', timeoutMs = uxplayControlTimeoutMs) {
  ensureUxplayWebSocketReady();
  const url = getWsUrl('127.0.0.1', uxplayWsPort);
  const authId = nextUxplayWsRequestId('auth');
  const requestId = nextUxplayWsRequestId(op);

  return new Promise((resolve, reject) => {
    let ws = null;
    let settled = false;
    let authenticated = false;
    let timeout = null;

    const cleanup = () => {
      if (timeout) {
        clearTimeout(timeout);
      }
      if (ws) {
        ws.removeAllListeners();
        if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
          ws.close();
        }
      }
    };

    const finish = (callback, value) => {
      if (settled) {
        return;
      }
      settled = true;
      cleanup();
      callback(value);
    };

    const fail = (error) => {
      finish(reject, error);
    };

    const sendRequest = (payload) => {
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        fail(
          createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay WebSocket control endpoint.', {
            httpStatus: 502,
          }),
        );
        return;
      }
      ws.send(JSON.stringify(payload), (error) => {
        if (error) {
          fail(
            createControlError('UPSTREAM_UNREACHABLE', 'Unable to send UxPlay WebSocket control request.', {
              httpStatus: 502,
              details: {
                cause: error.message,
              },
            }),
          );
        }
      });
    };

    timeout = setTimeout(() => {
      fail(
        createControlError('UPSTREAM_TIMEOUT', `UxPlay WebSocket ${op} request timed out.`, {
          httpStatus: 504,
          details: {
            op,
          },
        }),
      );
    }, Math.max(1, timeoutMs));

    ws = new WebSocket(url, {
      handshakeTimeout: Math.max(1, timeoutMs),
      maxPayload: uxplayWsResponseLimitBytes,
    });

    ws.on('open', () => {
      sendRequest({
        type: 'request',
        id: authId,
        op: 'auth',
        data: {
          token: controlSessionToken,
        },
      });
    });

    ws.on('message', (rawMessage) => {
      const rawLength = Buffer.isBuffer(rawMessage)
        ? rawMessage.length
        : Buffer.byteLength(String(rawMessage));
      if (rawLength > uxplayWsResponseLimitBytes) {
        fail(
          createControlError('UPSTREAM_RESPONSE_TOO_LARGE', 'UxPlay WebSocket response is too large.', {
            httpStatus: 502,
          }),
        );
        return;
      }

      let response;
      try {
        response = JSON.parse(rawMessage.toString('utf8'));
      } catch {
        fail(
          createControlError('UPSTREAM_INVALID_RESPONSE', 'UxPlay WebSocket response is not valid JSON.', {
            httpStatus: 502,
            details: {
              op,
            },
          }),
        );
        return;
      }

      if (!response || response.type !== 'response') {
        return;
      }

      if (response.id === authId) {
        if (!response.ok) {
          fail(makeUxplayWsRejectError(response, 'auth'));
          return;
        }
        authenticated = true;
        sendRequest({
          type: 'request',
          id: requestId,
          op,
          data,
        });
        return;
      }

      if (response.id !== requestId) {
        return;
      }

      if (!response.ok) {
        fail(makeUxplayWsRejectError(response, op));
        return;
      }

      finish(resolve, {
        statusCode: 200,
        body: response.data && typeof response.data === 'object'
          ? response.data
          : { message: fallbackMessage },
      });
    });

    ws.on('error', (error) => {
      fail(
        createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay WebSocket control endpoint.', {
          httpStatus: 502,
          details: {
            cause: error?.message || 'connection error',
            op,
          },
        }),
      );
    });

    ws.on('close', (code, reason) => {
      if (settled) {
        return;
      }
      fail(
        createControlError(
          authenticated ? 'UPSTREAM_REJECTED' : 'UPSTREAM_UNREACHABLE',
          'UxPlay WebSocket control endpoint closed before responding.',
          {
            httpStatus: 502,
            details: {
              closeCode: code,
              closeReason: reason?.toString('utf8') || '',
              op,
            },
          },
        ),
      );
    });
  });
}

async function requestUxplayWebSocket(op, data = {}, fallbackMessage = 'ok') {
  ensureUxplayWebSocketReady();
  const deadline = Date.now() + uxplayControlTimeoutMs;
  let lastError = null;

  do {
    const remainingMs = Math.max(1, deadline - Date.now());
    try {
      return await requestUxplayWebSocketOnce(op, data, fallbackMessage, remainingMs);
    } catch (error) {
      lastError = error;
      if (error?.code !== 'UPSTREAM_UNREACHABLE' || Date.now() >= deadline) {
        throw error;
      }
      await delay(Math.min(100, Math.max(1, deadline - Date.now())));
    }
  } while (Date.now() < deadline);

  throw lastError || createControlError('UPSTREAM_TIMEOUT', `UxPlay WebSocket ${op} request timed out.`, {
    httpStatus: 504,
    details: {
      op,
    },
  });
}

function requestUxplayPinUpdate(pin) {
  return requestUxplayWebSocket('setPin', { pin }, 'pin updated');
}

function requestUxplayAudioUpdate(enabled) {
  return requestUxplayWebSocket('setAudio', { enabled }, 'mirror audio updated');
}

function requestUxplayAudioStatus() {
  return requestUxplayWebSocket('getAudio', {}, 'mirror audio status');
}

function requestUxplayStop() {
  return requestUxplayWebSocket('stop', {}, 'casting stopped');
}

function updateCurrentPinFromUxplayEvent(rawPin, source) {
  const pin = normalizeOptionalPin(rawPin);
  if (!pin) {
    return null;
  }
  if (currentPin === pin) {
    return pin;
  }
  currentPin = pin;
  broadcastAppEvent('pin.changed', {
    port: uxplayHttpPort,
    pin,
    source,
  });
  broadcastAppEvent('pinChanged', {
    pin,
  });
  broadcastControlStatus();
  return pin;
}

function handleUxplayControlEvent(op, data = {}) {
  switch (op) {
    case 'pinRequired': {
      const pin = updateCurrentPinFromUxplayEvent(data?.pin, 'uxplay-pin-required');
      showPinWindow('uxplay-pin-required');
      broadcastAppEvent('pinRequired', {
        pin: pin || currentPin,
        reason: 'uxplay-event',
      });
      return;
    }

    case 'pinChanged':
      updateCurrentPinFromUxplayEvent(data?.pin, 'uxplay-pin-changed');
      return;

    case 'audioChanged':
      if (typeof data?.mirrorAudio === 'boolean' && mirrorAudioEnabled !== data.mirrorAudio) {
        mirrorAudioEnabled = data.mirrorAudio;
        broadcastAppEvent('audio.changed', {
          mirrorAudioEnabled,
          muted: !mirrorAudioEnabled,
          source: 'uxplay-event',
        });
        broadcastControlStatus();
      }
      return;

    default:
      return;
  }
}

function closeUxplayEventWebSocket(reason = 'unspecified') {
  if (uxplayEventReconnectTimer) {
    clearTimeout(uxplayEventReconnectTimer);
    uxplayEventReconnectTimer = null;
  }

  const ws = uxplayEventWs;
  uxplayEventWs = null;
  uxplayEventAuthenticated = false;
  if (!ws) {
    return;
  }
  ws.removeAllListeners();
  try {
    if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
      ws.close(1000, reason);
    }
  } catch {
    try {
      ws.terminate();
    } catch {
      // Ignore shutdown races.
    }
  }
}

function scheduleUxplayEventWebSocketReconnect(reason = 'unspecified', delayMs = 1000) {
  if (!uxplayWsEnabled || !bridge || uxplayEventReconnectTimer) {
    return;
  }
  if (
    uxplayEventWs &&
    (uxplayEventWs.readyState === WebSocket.OPEN || uxplayEventWs.readyState === WebSocket.CONNECTING)
  ) {
    return;
  }
  uxplayEventReconnectTimer = setTimeout(() => {
    uxplayEventReconnectTimer = null;
    connectUxplayEventWebSocket(reason);
  }, Math.max(0, delayMs));
}

function handleUxplayEventSocketMessage(ws, rawMessage, authId) {
  const rawLength = Buffer.isBuffer(rawMessage)
    ? rawMessage.length
    : Buffer.byteLength(String(rawMessage));
  if (rawLength > uxplayWsResponseLimitBytes) {
    ws.close(1009, 'message too large');
    return;
  }

  let message;
  try {
    message = JSON.parse(rawMessage.toString('utf8'));
  } catch {
    return;
  }

  if (!message || typeof message !== 'object') {
    return;
  }

  if (message.type === 'response' && message.id === authId) {
    if (!message.ok) {
      console.warn('[uxplay-ws-events] authentication failed');
      ws.close(1008, 'auth failed');
      return;
    }
    uxplayEventAuthenticated = true;
    return;
  }

  if (message.type !== 'event' || typeof message.op !== 'string') {
    return;
  }

  handleUxplayControlEvent(
    message.op,
    message.data && typeof message.data === 'object' ? message.data : {},
  );
}

function connectUxplayEventWebSocket(reason = 'manual') {
  if (!uxplayWsEnabled || !bridge) {
    return;
  }
  if (
    uxplayEventWs &&
    (uxplayEventWs.readyState === WebSocket.OPEN || uxplayEventWs.readyState === WebSocket.CONNECTING)
  ) {
    return;
  }

  const url = getWsUrl('127.0.0.1', uxplayWsPort);
  if (!url) {
    return;
  }

  const authId = nextUxplayWsRequestId('event-auth');
  const ws = new WebSocket(url, {
    handshakeTimeout: uxplayControlTimeoutMs,
    maxPayload: uxplayWsResponseLimitBytes,
  });
  uxplayEventWs = ws;
  uxplayEventAuthenticated = false;

  ws.on('open', () => {
    sendJsonToWebSocket(ws, {
      type: 'request',
      id: authId,
      op: 'auth',
      data: {
        token: controlSessionToken,
      },
    });
  });

  ws.on('message', (rawMessage) => {
    handleUxplayEventSocketMessage(ws, rawMessage, authId);
  });

  ws.on('error', (error) => {
    if (uxplayEventWs === ws && traceSharedTexture) {
      console.log(`[uxplay-ws-events] connection error (${reason}): ${error?.message || 'unknown error'}`);
    }
  });

  ws.on('close', () => {
    if (uxplayEventWs === ws) {
      uxplayEventWs = null;
      uxplayEventAuthenticated = false;
      scheduleUxplayEventWebSocketReconnect('event-socket-closed', 1000);
    }
  });
}

function persistEncryptedPin(pin, port) {
  if (!safeStorage.isEncryptionAvailable()) {
    throw createControlError(
      'ENCRYPTION_UNAVAILABLE',
      'OS secure encryption is unavailable; PIN cannot be persisted safely.',
      { httpStatus: 500 },
    );
  }
  const encryptedPin = safeStorage.encryptString(pin);
  const state = {
    version: 1,
    encryptedBy: 'electron.safeStorage',
    updatedAt: new Date().toISOString(),
    httpPort: port,
    wsPort: uxplayWsEnabled ? uxplayWsPort : null,
    ciphertext: encryptedPin.toString('base64'),
  };
  fs.writeFileSync(getPinStorePath(), `${JSON.stringify(state, null, 2)}\n`, 'utf8');
}

function restorePersistedPin() {
  const pinStorePath = getPinStorePath();
  if (!fs.existsSync(pinStorePath)) {
    return;
  }
  if (!safeStorage.isEncryptionAvailable()) {
    return;
  }
  try {
    const raw = fs.readFileSync(pinStorePath, 'utf8');
    const parsed = JSON.parse(raw);
    if (typeof parsed?.ciphertext !== 'string' || !parsed.ciphertext.length) {
      return;
    }
    const decrypted = safeStorage.decryptString(Buffer.from(parsed.ciphertext, 'base64'));
    if (/^\d{4}$/.test(decrypted)) {
      currentPin = decrypted;
    }
  } catch (error) {
    console.warn(`Failed to restore persisted pin state: ${error.message}`);
  }
}

async function applyPinUpdate(pin) {
  ensureUxplayWebSocketReady();
  const upstream = await requestUxplayPinUpdate(pin);
  persistEncryptedPin(pin, uxplayHttpPort);
  currentPin = pin;
  broadcastAppEvent('pin.changed', {
    port: uxplayHttpPort,
    pin,
  });
  broadcastAppEvent('pinChanged', {
    pin,
  });
  broadcastControlStatus();
  return {
    port: uxplayHttpPort,
    pin,
    result: upstream.body,
  };
}

async function refreshMirrorAudioStateFromUxPlay(trigger = 'manual') {
  try {
    ensureUxplayWebSocketReady();
    const upstream = await requestUxplayAudioStatus();
    const enabled = extractMirrorAudioEnabled(upstream.body);
    if (typeof enabled === 'boolean') {
      mirrorAudioEnabled = enabled;
      broadcastControlStatus();
    }
  } catch (error) {
    console.error(`[audio-control] failed to refresh mirror audio state (${trigger}): ${error?.message || 'unknown error'}`);
  }
}

async function applyMirrorAudioEnabled(enabled) {
  if (mirrorAudioUpdateInFlight) {
    throw createControlError('AUDIO_UPDATE_BUSY', 'Mirror audio update already in progress.', {
      httpStatus: 409,
    });
  }
  mirrorAudioUpdateInFlight = true;
  broadcastControlStatus();
  try {
    ensureUxplayWebSocketReady();
    const upstream = await requestUxplayAudioUpdate(enabled);
    const applied = extractMirrorAudioEnabled(upstream.body);
    mirrorAudioEnabled = typeof applied === 'boolean' ? applied : enabled;
    broadcastAppEvent('audio.changed', {
      mirrorAudioEnabled,
      muted: !mirrorAudioEnabled,
    });
    broadcastAppEvent('audioChanged', {
      mirrorAudio: mirrorAudioEnabled,
    });
    broadcastControlStatus();
    return {
      port: uxplayHttpPort,
      mirrorAudioEnabled,
      muted: !mirrorAudioEnabled,
      result: upstream.body,
    };
  } finally {
    mirrorAudioUpdateInFlight = false;
    broadcastControlStatus();
  }
}

async function rotatePinRandom(trigger = 'manual') {
  if (pinUpdateInFlight) {
    throw createControlError('PIN_UPDATE_BUSY', 'PIN update already in progress.', {
      httpStatus: 409,
    });
  }
  pinUpdateInFlight = true;
  broadcastControlStatus();
  try {
    const pin = generateRandomPin(currentPin);
    const updated = await applyPinUpdate(pin);
    return {
      ...updated,
      trigger,
    };
  } finally {
    pinUpdateInFlight = false;
    broadcastControlStatus();
  }
}

async function triggerAutoPinRotationOnMirrorStart() {
  if (autoPinRotatedForCastingSession) {
    return;
  }
  autoPinRotatedForCastingSession = true;
  try {
    const updated = await rotatePinRandom('auto');
    console.log(`[pin-control] auto rotated pin to ${updated.pin} on mirror session start`);
  } catch (error) {
    console.error(`[pin-control] auto pin rotation failed: ${error?.message || 'unknown error'}`);
    if (shouldRetryAutoPinUpdate(error)) {
      autoPinRotatedForCastingSession = false;
    }
  }
}

async function triggerAutoPinRotationOnPortReady() {
  try {
    const updated = await rotatePinRandom('auto-port-ready');
    mirrorSessionActive = true;
    touchMirrorSessionActivity();
    console.log(`[pin-control] auto rotated pin to ${updated.pin} when control port became available`);
  } catch (error) {
    console.error(`[pin-control] auto pin rotation on control port failed: ${error?.message || 'unknown error'}`);
  }
}

async function handleSetPinRequest(event, payload) {
  ensureTrustedSender(event);
  ensureControlToken(payload?.token);

  if (typeof payload?.pin === 'string' && payload.pin.trim()) {
    const manualPin = normalizePinInput(payload.pin);
    const updated = await applyPinUpdate(manualPin);
    return {
      ok: true,
      ...updated,
      trigger: 'manual-set',
    };
  }

  const updated = await rotatePinRandom('manual-random');
  return {
    ok: true,
    ...updated,
  };
}

async function handleRotatePinRequest(event, payload) {
  ensureTrustedSender(event);
  ensureControlToken(payload?.token);
  const updated = await rotatePinRandom('manual-random');
  return {
    ok: true,
    ...updated,
  };
}

async function handleSetMutedRequest(event, payload) {
  ensureTrustedSender(event);
  ensureControlToken(payload?.token);
  const muted = normalizeMutedInput(payload?.muted);
  const updated = await applyMirrorAudioEnabled(!muted);
  return {
    ok: true,
    ...updated,
  };
}

async function performStopCasting() {
  if (stopUpdateInFlight) {
    throw createControlError('STOP_UPDATE_BUSY', 'Stop casting request already in progress.', {
      httpStatus: 409,
    });
  }

  stopUpdateInFlight = true;
  broadcastControlStatus();
  try {
    ensureUxplayWebSocketReady();
    const upstream = await requestUxplayStop();
    mirrorSessionActive = false;
    clearMirrorSessionIdleTimer();
    setCastingActive(false, 'ws-stop-success', { clearPending: true });
    return {
      ok: true,
      port: uxplayHttpPort,
      result: upstream.body,
    };
  } finally {
    stopUpdateInFlight = false;
    broadcastControlStatus();
  }
}

async function handleStopCastingRequest(event, payload) {
  ensureTrustedSender(event);
  ensureControlToken(payload?.token);
  return performStopCasting();
}

function setCastWindowFullscreen(nextFullscreen, reason = 'manual') {
  if (!win || win.isDestroyed()) {
    throw createControlError('WINDOW_UNAVAILABLE', 'Application window is not available.', {
      httpStatus: 503,
    });
  }
  if (typeof nextFullscreen !== 'boolean') {
    throw createControlError('INVALID_FULLSCREEN_VALUE', 'Fullscreen must be a boolean value.', {
      httpStatus: 400,
    });
  }
  win.setFullScreen(nextFullscreen);
  broadcastAppEvent('window.changed', {
    window: 'cast',
    action: 'fullscreen',
    reason,
    state: getCastWindowState(),
  });
  broadcastControlStatus();
  return {
    isFullscreen: nextFullscreen,
  };
}

function sendAppWsResponse(ws, requestOrId, ok, data = {}, error = null) {
  const request = requestOrId && typeof requestOrId === 'object'
    ? requestOrId
    : { id: requestOrId, op: '' };
  const id = typeof request.id === 'undefined' || request.id === null ? '' : String(request.id);
  const method = typeof request.op === 'string' ? request.op : '';

  if (isAppWsAdvanced(ws)) {
    const status = {
      result: ok,
      code: ok ? 100 : 300,
    };
    if (!ok) {
      status.comment = error?.code || 'INTERNAL_ERROR';
    }
    const payload = {
      sid: ensureAppWsSessionId(ws, request.sid),
      op: 8,
      d: {
        id,
        status,
      },
    };
    if (method) {
      payload.d.method = method;
    }
    if (ok) {
      payload.d.result = data;
    }
    sendJsonToWebSocket(ws, payload);
    return;
  }

  const payload = {
    type: 'response',
    id,
    ok,
    data,
  };
  if (!ok) {
    payload.error = error?.code || 'INTERNAL_ERROR';
    payload.data = {
      ...(data && typeof data === 'object' ? data : {}),
      error: formatControlError(error),
    };
  }
  sendJsonToWebSocket(ws, payload);
}

function parseAppWsRequest(rawMessage) {
  let parsed;
  try {
    parsed = JSON.parse(rawMessage.toString('utf8'));
  } catch {
    throw createControlError('INVALID_JSON', 'Request payload must be valid JSON.', {
      httpStatus: 400,
    });
  }

  if (!parsed || typeof parsed !== 'object') {
    throw createControlError('INVALID_REQUEST', 'Request must contain type=request and op.', {
      httpStatus: 400,
    });
  }

  if (parsed.type === 'request' && typeof parsed.op === 'string') {
    return {
      protocolMode: 'legacy',
      id: parsed.id,
      op: parsed.op,
      data: parsed.data && typeof parsed.data === 'object' ? parsed.data : {},
    };
  }

  if (Number.isInteger(parsed.op)) {
    const sid = typeof parsed.sid === 'string' ? parsed.sid : null;
    if (parsed.op === 0) {
      return {
        protocolMode: 'advanced',
        controlOp: 'hello',
        sid,
      };
    }
    if (parsed.op === 14) {
      return {
        protocolMode: 'advanced',
        controlOp: 'bye',
        sid,
      };
    }
    if (parsed.op === 7 && parsed.d && typeof parsed.d === 'object' && typeof parsed.d.method === 'string') {
      return {
        protocolMode: 'advanced',
        sid,
        id: parsed.d.id,
        op: parsed.d.method,
        data: parsed.d.params && typeof parsed.d.params === 'object' ? parsed.d.params : {},
      };
    }
    return {
      protocolMode: 'advanced',
      sid,
      id: parsed.d && typeof parsed.d === 'object' ? parsed.d.id : undefined,
      op: '',
      invalidRequest: true,
    };
  }

  throw createControlError('INVALID_REQUEST', 'Request must be legacy type=request or advanced op=7.', {
    httpStatus: 400,
  });
}

function sendAppWsHelloAck(ws, sidHint = null) {
  ws.protocolMode = 'advanced';
  const sid = ensureAppWsSessionId(ws, sidHint);
  sendJsonToWebSocket(ws, {
    sid,
    op: 1,
    d: {
      protocolVersion: 2,
      serverName: uxplayServerName,
      capabilities: ['auth', 'request', 'event', 'bye'],
      legacyCompatible: true,
      maxPayload: 64 * 1024,
    },
  });
}

function sendAppWsByeAck(ws, sidHint = null) {
  ws.protocolMode = 'advanced';
  const sid = ensureAppWsSessionId(ws, sidHint);
  sendJsonToWebSocket(ws, {
    sid,
    op: 15,
    d: {
      message: 'bye',
    },
  });
}

async function stopBridge(reason = 'manual') {
  if (!bridge) {
    return;
  }

  const child = bridge;
  await new Promise((resolve) => {
    let settled = false;
    const finish = () => {
      if (settled) {
        return;
      }
      settled = true;
      resolve();
    };
    child.once('exit', finish);
    try {
      if (child.stdin?.writable) {
        child.stdin.write('STOP\n');
      } else {
        child.kill();
      }
    } catch {
      try {
        child.kill();
      } catch {
        // Ignore shutdown races.
      }
    }
    setTimeout(() => {
      if (!settled) {
        try {
          child.kill();
        } catch {
          // Ignore shutdown races.
        }
        if (bridge === child) {
          bridge = null;
          bridgeReady = false;
        }
      }
      finish();
    }, 3000);
  });
}

async function restartUxPlay(reason = 'manual') {
  await stopBridge(reason);
  startBridge();
  return getAppStatus();
}

async function handleAuthenticatedAppWsRequest(ws, request) {
  const data = request.data && typeof request.data === 'object' ? request.data : {};

  switch (request.op) {
    case 'getStatus':
      return getAppStatus();

    case 'getPin':
      return {
        pin: currentPin,
        updating: pinUpdateInFlight,
      };

    case 'rotatePin':
      return rotatePinRandom('ws-random');

    case 'setPin': {
      const pin = normalizePinInput(data.pin);
      return {
        ...(await applyPinUpdate(pin)),
        trigger: 'ws-set',
      };
    }

    case 'setMuted': {
      const muted = normalizeMutedInput(data.muted);
      return applyMirrorAudioEnabled(!muted);
    }

    case 'setAudio': {
      if (typeof data.enabled !== 'boolean') {
        throw createControlError('INVALID_AUDIO_VALUE', 'Audio enabled must be a boolean value.', {
          httpStatus: 400,
        });
      }
      return applyMirrorAudioEnabled(data.enabled);
    }

    case 'getAudio':
      return {
        mirrorAudioEnabled,
        muted: typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null,
        updating: mirrorAudioUpdateInFlight,
      };

    case 'stop':
    case 'stopCasting':
      return performStopCasting();

    case 'showCastWindow':
      showCastWindow('ws-command');
      broadcastControlStatus();
      return getAppStatus();

    case 'hideCastWindow':
      hideCastWindow('ws-command');
      broadcastControlStatus();
      return getAppStatus();

    case 'setFullscreen': {
      const fullscreen = data.fullscreen;
      return {
        ...setCastWindowFullscreen(fullscreen, 'ws-command'),
        status: getAppStatus(),
      };
    }

    case 'showPinWindow':
      showPinWindow('ws-command');
      return getAppStatus();

    case 'hidePinWindow':
      hidePinWindow('ws-command');
      return getAppStatus();

    case 'restartUxPlay':
      return restartUxPlay('ws-command');

    case 'quitApp':
      setImmediate(() => {
        quitApplication();
      });
      return {
        message: 'quitting',
      };

    default:
      throw createControlError('INVALID_OP', `Unknown operation: ${request.op}`, {
        httpStatus: 400,
      });
  }
}

async function handleAppWsMessage(ws, rawMessage) {
  let request;
  try {
    request = parseAppWsRequest(rawMessage);
    if (request.protocolMode === 'advanced') {
      ws.protocolMode = 'advanced';
      ensureAppWsSessionId(ws, request.sid);
    }

    if (request.controlOp === 'hello') {
      sendAppWsHelloAck(ws, request.sid);
      return;
    }

    if (request.controlOp === 'bye') {
      sendAppWsByeAck(ws, request.sid);
      ws.close(1000, 'bye');
      return;
    }

    if (request.invalidRequest) {
      throw createControlError('INVALID_OP', 'Unknown advanced WebSocket opcode.', {
        httpStatus: 400,
      });
    }

    if (!ws.isAuthenticated) {
      if (request.op !== 'auth') {
        throw createControlError('UNAUTHORIZED', 'First request must be auth.', {
          httpStatus: 401,
        });
      }
      if (request.data?.token !== appWsToken) {
        throw createControlError('UNAUTHORIZED', 'Invalid WebSocket control token.', {
          httpStatus: 401,
        });
      }
      ws.isAuthenticated = true;
      sendAppWsResponse(ws, request, true, { message: 'authenticated' });
      broadcastAppEvent('app.ready', getAppStatus());
      return;
    }

    if (request.op === 'auth') {
      sendAppWsResponse(ws, request, true, { message: 'authenticated' });
      return;
    }

    const result = await handleAuthenticatedAppWsRequest(ws, request);
    sendAppWsResponse(ws, request, true, result);
  } catch (error) {
    const formatted = rememberLastError(error);
    broadcastAppEvent('error', formatted);
    sendAppWsResponse(ws, request || undefined, false, {}, error);
    if (error?.code === 'UNAUTHORIZED') {
      ws.close(1008, 'unauthorized');
    }
  }
}

function startAppWebSocketServer() {
  if (!appWsEnabled || appWsServer) {
    return;
  }

  appWsServer = new WebSocket.Server({
    host: appWsHost,
    port: appWsPort,
    maxPayload: 64 * 1024,
  });

  appWsServer.on('connection', (ws) => {
    ws.isAuthenticated = false;
    ws.protocolMode = 'legacy';
    ws.sessionId = null;
    appWsClients.add(ws);
    ws.on('message', (message) => {
      void handleAppWsMessage(ws, message);
    });
    ws.on('close', () => {
      appWsClients.delete(ws);
      broadcastControlStatus();
    });
    ws.on('error', (error) => {
      rememberLastError(error, 'WS_CLIENT_ERROR');
      appWsClients.delete(ws);
      broadcastControlStatus();
    });
    broadcastControlStatus();
  });

  appWsServer.on('listening', () => {
    console.log(`Electron WebSocket control server listening on ${getWsUrl(appWsHost, appWsPort)}`);
    broadcastAppEvent('app.ready', getAppStatus());
    broadcastControlStatus();
  });

  appWsServer.on('error', (error) => {
    const formatted = rememberLastError(error, 'WS_SERVER_ERROR');
    console.error(`Electron WebSocket control server failed: ${formatted.message}`);
    broadcastAppEvent('error', formatted);
    broadcastControlStatus();
  });
}

function stopAppWebSocketServer() {
  for (const client of appWsClients) {
    try {
      client.close(1001, 'app quitting');
    } catch {
      // Ignore shutdown races.
    }
  }
  appWsClients.clear();
  if (appWsServer) {
    appWsServer.close();
    appWsServer = null;
  }
}

function setupIpcHandlers() {
  ipcMain.handle('uxplay-control:get-session', (event) => {
    try {
      ensureTrustedSender(event);
      return {
        ok: true,
        ...getAppStatus(),
        token: controlSessionToken,
      };
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('uxplay-control:set-pin', async (event, payload) => {
    try {
      return await handleSetPinRequest(event, payload);
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('uxplay-control:rotate-pin', async (event, payload) => {
    try {
      return await handleRotatePinRequest(event, payload);
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('uxplay-control:set-muted', async (event, payload) => {
    try {
      return await handleSetMutedRequest(event, payload);
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('uxplay-control:stop-casting', async (event, payload) => {
    try {
      return await handleStopCastingRequest(event, payload);
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('window-control:get-state', (event) => {
    try {
      ensureTrustedSender(event);
      const castWindowState = getCastWindowState();
      return {
        ok: true,
        isFullscreen: castWindowState.fullscreen,
        visible: castWindowState.visible,
      };
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('window-control:toggle-fullscreen', (event, payload) => {
    try {
      ensureTrustedSender(event);
      ensureControlToken(payload?.token);
      if (!win || win.isDestroyed()) {
        throw createControlError('WINDOW_UNAVAILABLE', 'Application window is not available.', {
          httpStatus: 503,
        });
      }
      const nextFullscreen = !win.isFullScreen();
      setCastWindowFullscreen(nextFullscreen, 'ipc-toggle');
      return {
        ok: true,
        isFullscreen: nextFullscreen,
      };
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });

  ipcMain.handle('window-control:set-windowed', (event, payload) => {
    try {
      ensureTrustedSender(event);
      ensureControlToken(payload?.token);
      if (!win || win.isDestroyed()) {
        throw createControlError('WINDOW_UNAVAILABLE', 'Application window is not available.', {
          httpStatus: 503,
        });
      }
      setCastWindowFullscreen(false, 'ipc-windowed');
      return {
        ok: true,
        isFullscreen: false,
      };
    } catch (error) {
      return {
        ok: false,
        error: formatControlError(error),
      };
    }
  });
}

function createWindow() {
  Menu.setApplicationMenu(null);
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 540,
    backgroundColor: '#000000',
    frame: false,
    autoHideMenuBar: true,
    show: false,
    fullscreen: false,
    fullscreenable: true,
    skipTaskbar: true,
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
    },
  });

  win.removeMenu();
  win.setMenuBarVisibility(false);
  win.loadFile(path.join(__dirname, 'index.html'));
  win.webContents.on('console-message', (_event, levelOrDetails, message, line, sourceId) => {
    if (typeof levelOrDetails === 'object' && levelOrDetails) {
      console.log(
        `[renderer:${levelOrDetails.level}] ${levelOrDetails.message} ` +
          `(${levelOrDetails.sourceId}:${levelOrDetails.line})`,
      );
      return;
    }

    console.log(`[renderer:${levelOrDetails}] ${message} (${sourceId}:${line})`);
  });
  win.on('enter-full-screen', () => {
    broadcastAppEvent('window.changed', {
      window: 'cast',
      action: 'enter-fullscreen',
      state: getCastWindowState(),
    });
    broadcastControlStatus();
  });
  win.on('leave-full-screen', () => {
    broadcastAppEvent('window.changed', {
      window: 'cast',
      action: 'leave-fullscreen',
      state: getCastWindowState(),
    });
    broadcastControlStatus();
  });
  win.on('show', () => {
    updateTrayMenu();
    broadcastControlStatus();
  });
  win.on('hide', () => {
    updateTrayMenu();
    broadcastControlStatus();
  });
  win.on('close', (event) => {
    if (isExplicitlyQuitting) {
      return;
    }
    event.preventDefault();
    hideCastWindow('window-close');
  });
  win.on('closed', () => {
    win = null;
  });
}

function handleToBuffer(handleHex) {
  const value = BigInt(handleHex);
  const buffer = Buffer.alloc(8);
  buffer.writeBigUInt64LE(value);
  return buffer;
}

function traceSharedTextureStats(reason) {
  if (!traceSharedTexture) {
    return;
  }

  console.log(
    `[shared-texture:${reason}] received=${framesReceived} queued=${framesQueued} ` +
      `sent=${framesSent} released=${frameReleases} importFailures=${importFailures} ` +
      `sendFailures=${sendFailures} sendTimeouts=${sendTimeouts} ` +
      `pending=${pendingFrame ? pendingFrame.frameId : 'none'} ` +
      `sending=${frameSending}`,
  );
}

function notifyBridgeRelease(frameId) {
  if (!bridge || !bridge.stdin.writable) {
    return;
  }
  bridge.stdin.write(`RELEASE\t${frameId}\n`);
}

function releasePendingFrame(frame, options = {}) {
  if (!frame) {
    return;
  }

  const { notifyRelease = false } = options;

  try {
    frame.importedSharedTexture.release();
  } catch {
    // Ignore double release during shutdown.
  }

  // For frames dropped before sendSharedTexture(), proactively release the slot
  // in UxPlay instead of waiting on allReferencesReleased callback semantics.
  if (notifyRelease) {
    notifyBridgeRelease(frame.frameId);
  }
}

function queueFrame(frame) {
  if (!castingActive) {
    releasePendingFrame(frame, { notifyRelease: true });
    return;
  }

  if (pendingFrame) {
    releasePendingFrame(pendingFrame, { notifyRelease: true });
  }

  pendingFrame = frame;
  framesQueued += 1;
  if (traceSharedTexture && (framesQueued <= 5 || framesQueued % 30 === 0)) {
    traceSharedTextureStats(`queue-${frame.frameId}`);
  }
  void flushFrameQueue();
}

async function sendFrameToRenderer(frame) {
  const sendPromise = sharedTexture.sendSharedTexture(
    {
      frame: win.webContents.mainFrame,
      importedSharedTexture: frame.importedSharedTexture,
    },
    {
      width: frame.width,
      height: frame.height,
      timestampUs: frame.timestampUs,
      frameId: frame.frameId,
    },
  );

  if (sendTimeoutMs <= 0) {
    await sendPromise;
    return;
  }

  let timedOut = false;
  let timeoutHandle;

  try {
    await Promise.race([
      sendPromise,
      new Promise((_, reject) => {
        timeoutHandle = setTimeout(() => {
          timedOut = true;
          reject(new Error(`sendSharedTexture timed out after ${sendTimeoutMs}ms`));
        }, sendTimeoutMs);
      }),
    ]);
  } catch (error) {
    if (timedOut) {
      sendTimeouts += 1;
      // The send promise may settle later; absorb it to avoid unhandled rejections.
      sendPromise.catch(() => {});
    }
    throw error;
  } finally {
    if (timeoutHandle) {
      clearTimeout(timeoutHandle);
    }
  }
}

async function flushFrameQueue() {
  if (frameSending || !pendingFrame || !win || win.isDestroyed() || !bridgeReady || !castingActive) {
    return;
  }

  frameSending = true;

  while (pendingFrame && win && !win.isDestroyed()) {
    const frame = pendingFrame;
    pendingFrame = null;
    let sentToRenderer = false;

    if (!castingActive) {
      releasePendingFrame(frame, { notifyRelease: true });
      continue;
    }

    try {
      await sendFrameToRenderer(frame);
      sentToRenderer = true;
      framesSent += 1;
      if (traceSharedTexture && (framesSent <= 5 || framesSent % 30 === 0)) {
        traceSharedTextureStats(`sent-${frame.frameId}`);
      }
    } catch (error) {
      sendFailures += 1;
      console.error('sendSharedTexture failed:', error);
      traceSharedTextureStats(`send-failed-${frame.frameId}`);
    } finally {
      releasePendingFrame(frame, { notifyRelease: !sentToRenderer });
    }
  }

  frameSending = false;
}

function startBridge() {
  if (bridge) {
    return;
  }
  if (!fs.existsSync(uxplayExecutable)) {
    throw new Error(
      `UxPlay executable not found: ${uxplayExecutable}. Build UxPlay first or set UXPLAY_EXE.`,
    );
  }

  const extraArgs = splitCommandLineArgs(process.env.UXPLAY_ARGS || '');
  const childArgs = [
    '-n',
    uxplayServerName,
    '-stpid',
    String(process.pid),
    // '-stdoutlog',
    // '7',
    // '-d',
    '-pw',
    '-nh',
    '-fs',
    '-nohold',
    ...(uxplayWsEnabled ? ['-ws-enable', '-ws-port', String(uxplayWsPort)] : []),
    '-logfile',
    ...extraArgs,
  ];

  const childEnv = {
    ...process.env,
    GST_PLUGIN_PATH: gstreamerPluginPath,
    UXPLAY_CONTROL_TOKEN: controlSessionToken,
    PATH: `${uxplayWorkdir};${gstreamerBin};${process.env.PATH || ''}`,
  };
  if (!childEnv.GST_PLUGIN_SCANNER && fs.existsSync(gstreamerPluginScanner)) {
    childEnv.GST_PLUGIN_SCANNER = gstreamerPluginScanner;
  }
  if (app.isPackaged) {
    childEnv.GST_PLUGIN_SYSTEM_PATH = childEnv.GST_PLUGIN_SYSTEM_PATH || '';
    childEnv.GST_PLUGIN_SYSTEM_PATH_1_0 = childEnv.GST_PLUGIN_SYSTEM_PATH_1_0 || '';
  }

  bridge = spawn(
    uxplayExecutable,
    childArgs,
    {
      cwd: uxplayWorkdir,
      env: childEnv,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    },
  );

  bridge.on('error', (error) => {
    const formatted = rememberLastError(error, 'UXPLAY_START_FAILED');
    broadcastAppEvent('error', formatted);
    broadcastControlStatus();
    console.error('Failed to launch UxPlay:', error);
  });

  bridge.on('exit', (code, signal) => {
    console.error(`UxPlay exited code=${code} signal=${signal}`);
    closeUxplayEventWebSocket('bridge-exit');
    bridge = null;
    bridgeReady = false;
    uxplayHttpPort = null;
    uxplayStderrBuffer = '';
    pinUpdateInFlight = false;
    mirrorAudioEnabled = null;
    mirrorAudioUpdateInFlight = false;
    stopUpdateInFlight = false;
    setCastingActive(false, 'bridge-exit', { clearPending: true });
    mirrorSessionActive = false;
    clearMirrorSessionIdleTimer();
    broadcastAppEvent('uxplay.exited', {
      code,
      signal,
    });
    broadcastControlStatus();
  });

  bridgeReader = readline.createInterface({
    input: bridge.stdout,
    crlfDelay: Infinity,
  });

  bridgeReader.on('line', (line) => {
    if (!line) {
      return;
    }

    if (line === 'READY') {
      bridgeReady = true;
      broadcastAppEvent('uxplay.ready', getAppStatus());
      broadcastControlStatus();
      scheduleUxplayEventWebSocketReconnect('uxplay-ready', 0);
      console.log(`UxPlay shared texture export is ready. AirPlay server name: ${uxplayServerName}`);
      void flushFrameQueue();
      return;
    }

    const [type, ...rest] = line.split('\t');
    if (type === 'SESSION') {
      const [sessionState, ...reasonParts] = rest;
      const reason = reasonParts.length > 0 ? reasonParts.join('\t') : 'bridge';
      if (sessionState === 'ACTIVE') {
        setCastingActive(true, `session-active:${reason}`);
      } else if (sessionState === 'INACTIVE') {
        mirrorSessionActive = false;
        clearMirrorSessionIdleTimer();
        setCastingActive(false, `session-inactive:${reason}`, { clearPending: true });
      } else {
        console.log(`[uxplay] ${line}`);
      }
      return;
    }

    if (type !== 'FRAME' || rest.length < 5) {
      updateHttpPortFromLogLine(line);
      console.log(`[uxplay] ${line}`);
      return;
    }

    const [frameIdText, widthText, heightText, timestampText, handleHex] = rest;
    const frameId = Number(frameIdText);
    const width = Number(widthText);
    const height = Number(heightText);
    const timestampUs = Number(timestampText);
    if (!castingActive) {
      setCastingActive(true, 'frame-fallback');
    }
    framesReceived += 1;
    touchMirrorSessionActivity();
    if (framesReceived <= 3 || framesReceived % 60 === 0) {
      broadcastAppEvent('casting.frameStats', getFrameStats());
    }
    if (traceSharedTexture && (framesReceived <= 5 || framesReceived % 30 === 0)) {
      traceSharedTextureStats(`recv-${frameId}`);
    }

    let importedSharedTexture;
    try {
      importedSharedTexture = sharedTexture.importSharedTexture({
        textureInfo: {
          pixelFormat: 'bgra',
          codedSize: { width, height },
          visibleRect: { x: 0, y: 0, width, height },
          timestamp: timestampUs,
          handle: {
            ntHandle: handleToBuffer(handleHex),
          },
        },
        allReferencesReleased: () => {
          frameReleases += 1;
          notifyBridgeRelease(frameId);
          if (traceSharedTexture && (frameReleases <= 5 || frameReleases % 30 === 0)) {
            traceSharedTextureStats(`release-${frameId}`);
          }
        },
      });
    } catch (error) {
      importFailures += 1;
      console.error('importSharedTexture failed:', error);
      notifyBridgeRelease(frameId);
      traceSharedTextureStats(`import-failed-${frameId}`);
      return;
    }

    queueFrame({
      frameId,
      width,
      height,
      timestampUs,
      importedSharedTexture,
    });
  });

  bridge.stderr.on('data', (chunk) => {
    consumeUxplayStderrChunk(chunk);
    process.stderr.write(chunk);
  });

  scheduleUxplayEventWebSocketReconnect('bridge-start', 250);
}

if (gotSingleInstanceLock) {
  app.on('second-instance', () => {
    if (castingActive) {
      showCastWindow('second-instance');
    }
  });

  app.whenReady().then(() => {
    restorePersistedPin();
    setupIpcHandlers();
    createWindow();
    createPinWindow();
    createTray();
    appReady = true;
    startAppWebSocketServer();
    broadcastAppEvent('app.ready', getAppStatus());
    broadcastControlStatus();
    if (sendTimeoutMs > 0) {
      console.log(`sendSharedTexture watchdog timeout: ${sendTimeoutMs}ms`);
    } else {
      console.log('sendSharedTexture watchdog disabled');
    }

    win.webContents.once('did-finish-load', () => {
      try {
        startBridge();
      } catch (error) {
        const formatted = rememberLastError(error, 'UXPLAY_START_FAILED');
        console.error(`Failed to start UxPlay bridge: ${formatted.message}`);
        broadcastAppEvent('error', formatted);
        broadcastControlStatus();
      }
    });
  });

  app.on('window-all-closed', () => {
    if (isExplicitlyQuitting) {
      app.quit();
    }
  });

  app.on('before-quit', () => {
    isExplicitlyQuitting = true;
    stopAppWebSocketServer();
    closeUxplayEventWebSocket('app-quitting');
    if (bridge && bridge.stdin.writable) {
      bridge.stdin.write('STOP\n');
    }
    clearMirrorSessionIdleTimer();
    setCastingActive(false, 'before-quit', { clearPending: true });
  });
}
