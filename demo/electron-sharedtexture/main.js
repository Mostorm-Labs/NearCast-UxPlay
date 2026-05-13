const { app, BrowserWindow, Menu, Tray, ipcMain, nativeImage, safeStorage, sharedTexture } = require('electron');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const readline = require('node:readline');

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
const uxplayWsPort = Number.parseInt(process.env.UXPLAY_WS_PORT || '7001', 10) || 7001;
const uxplayWsEnabled = process.env.UXPLAY_WS_ENABLE !== '0' && process.env.UXPLAY_WS_ENABLE !== 'false';
const mirrorSessionIdleMs = Number.parseInt(process.env.UXPLAY_MIRROR_IDLE_MS || '3000', 10) || 3000;
const castWindowFullscreen =
  process.env.UXPLAY_CAST_FULLSCREEN !== '0' &&
  process.env.UXPLAY_CAST_FULLSCREEN !== 'false';
const uxplayPinStoreFileName = 'uxplay-pin-state.json';
const controlSessionToken = crypto.randomBytes(24).toString('hex');
const serverPortLogPattern = /Initialized server socket\(s\) on port (\d{1,5})/;

let win;
let tray;
let isExplicitlyQuitting = false;
let bridge;
let bridgeReader;
let bridgeReady = false;
let uxplayHttpPort = null;
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

function broadcastControlStatus() {
  if (!win || win.isDestroyed()) {
    return;
  }
  const castWindowState = getCastWindowState();
  win.webContents.send('uxplay-control:status', {
    ready: bridgeReady,
    port: uxplayHttpPort,
    wsPort: uxplayWsEnabled ? uxplayWsPort : null,
    wsUrl: uxplayWsEnabled ? `ws://127.0.0.1:${uxplayWsPort}/` : null,
    castingActive,
    pin: currentPin,
    rotating: pinUpdateInFlight,
    mirrorAudioEnabled,
    muted: typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null,
    audioUpdating: mirrorAudioUpdateInFlight,
    stopUpdating: stopUpdateInFlight,
    isFullscreen: castWindowState.fullscreen,
    windowVisible: castWindowState.visible,
    castWindowVisible: castWindowState.visible,
  });
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
      broadcastControlStatus();
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

function ensureControlPortReady() {
  if (!Number.isInteger(uxplayHttpPort) || uxplayHttpPort <= 0 || uxplayHttpPort > 65535) {
    throw createControlError('HTTP_PORT_UNAVAILABLE', 'UxPlay HTTP control port is not ready yet.', {
      httpStatus: 503,
    });
  }
}

function shouldRetryAutoPinUpdate(error) {
  return (
    error?.code === 'HTTP_PORT_UNAVAILABLE' ||
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
    mirrorSessionIdleTimer = null;
    mirrorSessionActive = false;
    setCastingActive(false, 'mirror-idle-timeout', { clearPending: true });
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
  updateTrayMenu();
  if (traceSharedTexture) {
    console.log(`[window] cast window hidden reason=${reason}`);
  }
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
    showCastWindow(reason);
    void triggerAutoPinRotationOnMirrorStart();
  } else {
    mirrorSessionActive = false;
    autoPinRotatedForCastingSession = false;
    clearMirrorSessionIdleTimer();
    hideCastWindow(reason);
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

function requestUxplayPinUpdate(port, pin) {
  return new Promise((resolve, reject) => {
    const body = JSON.stringify({ pin });
    const req = http.request(
      {
        host: '127.0.0.1',
        port,
        path: '/api/pin',
        method: 'PUT',
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(body),
        },
        timeout: uxplayControlTimeoutMs,
      },
      (res) => {
        const chunks = [];
        let totalLength = 0;
        res.on('data', (chunk) => {
          totalLength += chunk.length;
          if (totalLength > 16384) {
            req.destroy(
              createControlError('UPSTREAM_RESPONSE_TOO_LARGE', 'UxPlay response is too large.', {
                httpStatus: 502,
              }),
            );
            return;
          }
          chunks.push(chunk);
        });
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8').trim();
          let json = null;
          if (text) {
            try {
              json = JSON.parse(text);
            } catch {
              json = null;
            }
          }
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 300) {
            resolve({
              statusCode: res.statusCode,
              body: json || { message: text || 'pin updated' },
            });
            return;
          }

          const errorMessage =
            json?.message ||
            text ||
            `UxPlay pin update failed with HTTP ${res.statusCode || 'unknown'}.`;
          reject(
            createControlError('UPSTREAM_REJECTED', errorMessage, {
              httpStatus: 502,
              details: {
                upstreamStatusCode: res.statusCode || 0,
              },
            }),
          );
        });
      },
    );

    req.on('timeout', () => {
      req.destroy(
        createControlError('UPSTREAM_TIMEOUT', 'UxPlay pin update request timed out.', {
          httpStatus: 504,
        }),
      );
    });
    req.on('error', (error) => {
      if (
        error?.code === 'UPSTREAM_TIMEOUT' ||
        error?.code === 'UPSTREAM_RESPONSE_TOO_LARGE' ||
        error?.code === 'UPSTREAM_REJECTED'
      ) {
        reject(error);
        return;
      }
      reject(
        createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay control endpoint.', {
          httpStatus: 502,
          details: {
            cause: error?.message || 'connection error',
          },
        }),
      );
    });
    req.write(body);
    req.end();
  });
}

function requestUxplayAudioUpdate(port, enabled) {
  return new Promise((resolve, reject) => {
    const body = JSON.stringify({ enabled });
    const req = http.request(
      {
        host: '127.0.0.1',
        port,
        path: '/api/audio',
        method: 'PUT',
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(body),
        },
        timeout: uxplayControlTimeoutMs,
      },
      (res) => {
        const chunks = [];
        let totalLength = 0;
        res.on('data', (chunk) => {
          totalLength += chunk.length;
          if (totalLength > 16384) {
            req.destroy(
              createControlError('UPSTREAM_RESPONSE_TOO_LARGE', 'UxPlay response is too large.', {
                httpStatus: 502,
              }),
            );
            return;
          }
          chunks.push(chunk);
        });
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8').trim();
          let json = null;
          if (text) {
            try {
              json = JSON.parse(text);
            } catch {
              json = null;
            }
          }
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 300) {
            resolve({
              statusCode: res.statusCode,
              body: json || { message: text || 'mirror audio updated' },
            });
            return;
          }

          const errorMessage =
            json?.message ||
            text ||
            `UxPlay mirror audio update failed with HTTP ${res.statusCode || 'unknown'}.`;
          reject(
            createControlError('UPSTREAM_REJECTED', errorMessage, {
              httpStatus: 502,
              details: {
                upstreamStatusCode: res.statusCode || 0,
              },
            }),
          );
        });
      },
    );

    req.on('timeout', () => {
      req.destroy(
        createControlError('UPSTREAM_TIMEOUT', 'UxPlay mirror audio update request timed out.', {
          httpStatus: 504,
        }),
      );
    });
    req.on('error', (error) => {
      if (
        error?.code === 'UPSTREAM_TIMEOUT' ||
        error?.code === 'UPSTREAM_RESPONSE_TOO_LARGE' ||
        error?.code === 'UPSTREAM_REJECTED'
      ) {
        reject(error);
        return;
      }
      reject(
        createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay control endpoint.', {
          httpStatus: 502,
          details: {
            cause: error?.message || 'connection error',
          },
        }),
      );
    });
    req.write(body);
    req.end();
  });
}

function requestUxplayAudioStatus(port) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      {
        host: '127.0.0.1',
        port,
        path: '/api/audio',
        method: 'GET',
        timeout: uxplayControlTimeoutMs,
      },
      (res) => {
        const chunks = [];
        let totalLength = 0;
        res.on('data', (chunk) => {
          totalLength += chunk.length;
          if (totalLength > 16384) {
            req.destroy(
              createControlError('UPSTREAM_RESPONSE_TOO_LARGE', 'UxPlay response is too large.', {
                httpStatus: 502,
              }),
            );
            return;
          }
          chunks.push(chunk);
        });
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8').trim();
          let json = null;
          if (text) {
            try {
              json = JSON.parse(text);
            } catch {
              json = null;
            }
          }
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 300) {
            resolve({
              statusCode: res.statusCode,
              body: json || { message: text || 'mirror audio status' },
            });
            return;
          }

          const errorMessage =
            json?.message ||
            text ||
            `UxPlay mirror audio status failed with HTTP ${res.statusCode || 'unknown'}.`;
          reject(
            createControlError('UPSTREAM_REJECTED', errorMessage, {
              httpStatus: 502,
              details: {
                upstreamStatusCode: res.statusCode || 0,
              },
            }),
          );
        });
      },
    );

    req.on('timeout', () => {
      req.destroy(
        createControlError('UPSTREAM_TIMEOUT', 'UxPlay mirror audio status request timed out.', {
          httpStatus: 504,
        }),
      );
    });
    req.on('error', (error) => {
      if (
        error?.code === 'UPSTREAM_TIMEOUT' ||
        error?.code === 'UPSTREAM_RESPONSE_TOO_LARGE' ||
        error?.code === 'UPSTREAM_REJECTED'
      ) {
        reject(error);
        return;
      }
      reject(
        createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay control endpoint.', {
          httpStatus: 502,
          details: {
            cause: error?.message || 'connection error',
          },
        }),
      );
    });
    req.end();
  });
}

function requestUxplayStop(port) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      {
        host: '127.0.0.1',
        port,
        path: '/api/stop',
        method: 'POST',
        timeout: uxplayControlTimeoutMs,
      },
      (res) => {
        const chunks = [];
        let totalLength = 0;
        res.on('data', (chunk) => {
          totalLength += chunk.length;
          if (totalLength > 16384) {
            req.destroy(
              createControlError('UPSTREAM_RESPONSE_TOO_LARGE', 'UxPlay response is too large.', {
                httpStatus: 502,
              }),
            );
            return;
          }
          chunks.push(chunk);
        });
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8').trim();
          let json = null;
          if (text) {
            try {
              json = JSON.parse(text);
            } catch {
              json = null;
            }
          }
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 300) {
            resolve({
              statusCode: res.statusCode,
              body: json || { message: text || 'casting stopped' },
            });
            return;
          }

          const errorMessage =
            json?.message ||
            text ||
            `UxPlay stop request failed with HTTP ${res.statusCode || 'unknown'}.`;
          reject(
            createControlError('UPSTREAM_REJECTED', errorMessage, {
              httpStatus: 502,
              details: {
                upstreamStatusCode: res.statusCode || 0,
              },
            }),
          );
        });
      },
    );

    req.on('timeout', () => {
      req.destroy(
        createControlError('UPSTREAM_TIMEOUT', 'UxPlay stop request timed out.', {
          httpStatus: 504,
        }),
      );
    });
    req.on('error', (error) => {
      if (
        error?.code === 'UPSTREAM_TIMEOUT' ||
        error?.code === 'UPSTREAM_RESPONSE_TOO_LARGE' ||
        error?.code === 'UPSTREAM_REJECTED'
      ) {
        reject(error);
        return;
      }
      reject(
        createControlError('UPSTREAM_UNREACHABLE', 'Unable to reach UxPlay control endpoint.', {
          httpStatus: 502,
          details: {
            cause: error?.message || 'connection error',
          },
        }),
      );
    });
    req.end();
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
  ensureControlPortReady();
  const upstream = await requestUxplayPinUpdate(uxplayHttpPort, pin);
  persistEncryptedPin(pin, uxplayHttpPort);
  currentPin = pin;
  broadcastControlStatus();
  return {
    port: uxplayHttpPort,
    pin,
    result: upstream.body,
  };
}

async function refreshMirrorAudioStateFromUxPlay(trigger = 'manual') {
  try {
    ensureControlPortReady();
    const upstream = await requestUxplayAudioStatus(uxplayHttpPort);
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
    ensureControlPortReady();
    const upstream = await requestUxplayAudioUpdate(uxplayHttpPort, enabled);
    const applied = extractMirrorAudioEnabled(upstream.body);
    mirrorAudioEnabled = typeof applied === 'boolean' ? applied : enabled;
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

async function handleStopCastingRequest(event, payload) {
  ensureTrustedSender(event);
  ensureControlToken(payload?.token);
  if (stopUpdateInFlight) {
    throw createControlError('STOP_UPDATE_BUSY', 'Stop casting request already in progress.', {
      httpStatus: 409,
    });
  }

  stopUpdateInFlight = true;
  broadcastControlStatus();
  try {
    ensureControlPortReady();
    const upstream = await requestUxplayStop(uxplayHttpPort);
    mirrorSessionActive = false;
    clearMirrorSessionIdleTimer();
    setCastingActive(false, 'http-stop-success', { clearPending: true });
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

function setupIpcHandlers() {
  ipcMain.handle('uxplay-control:get-session', (event) => {
    try {
      ensureTrustedSender(event);
      const castWindowState = getCastWindowState();
      return {
        ok: true,
        token: controlSessionToken,
        port: uxplayHttpPort,
        wsPort: uxplayWsEnabled ? uxplayWsPort : null,
        wsUrl: uxplayWsEnabled ? `ws://127.0.0.1:${uxplayWsPort}/` : null,
        ready: bridgeReady,
        castingActive,
        pin: currentPin,
        rotating: pinUpdateInFlight,
        mirrorAudioEnabled,
        muted: typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null,
        audioUpdating: mirrorAudioUpdateInFlight,
        stopUpdating: stopUpdateInFlight,
        isFullscreen: castWindowState.fullscreen,
        windowVisible: castWindowState.visible,
        castWindowVisible: castWindowState.visible,
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
      win.setFullScreen(nextFullscreen);
      broadcastControlStatus();
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
      win.setFullScreen(false);
      broadcastControlStatus();
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
    broadcastControlStatus();
  });
  win.on('leave-full-screen', () => {
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
    console.error('Failed to launch UxPlay:', error);
  });

  bridge.on('exit', (code, signal) => {
    console.error(`UxPlay exited code=${code} signal=${signal}`);
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
      broadcastControlStatus();
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
    createTray();
    broadcastControlStatus();
    if (sendTimeoutMs > 0) {
      console.log(`sendSharedTexture watchdog timeout: ${sendTimeoutMs}ms`);
    } else {
      console.log('sendSharedTexture watchdog disabled');
    }

    win.webContents.once('did-finish-load', () => {
      startBridge();
    });
  });

  app.on('window-all-closed', () => {
    if (isExplicitlyQuitting) {
      app.quit();
    }
  });

  app.on('before-quit', () => {
    isExplicitlyQuitting = true;
    if (bridge && bridge.stdin.writable) {
      bridge.stdin.write('STOP\n');
    }
    clearMirrorSessionIdleTimer();
    setCastingActive(false, 'before-quit', { clearPending: true });
  });
}
