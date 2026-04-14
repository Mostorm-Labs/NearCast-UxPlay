const { app, BrowserWindow, ipcMain, safeStorage, sharedTexture } = require('electron');
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
const uxplayExecutable =
  process.env.UXPLAY_EXE || path.join(repoRoot, 'build', 'uxplay.exe');
const uxplayWorkdir = path.dirname(uxplayExecutable);
const uxplayServerName =
  process.env.UXPLAY_SERVER_NAME || 'UxPlay SharedTexture';
const msysRoot = process.env.MSYS64_ROOT || 'D:\\msys64';
const gstreamerBin = path.join(msysRoot, 'mingw64', 'bin');
const gstreamerPluginPath =
  process.env.GST_PLUGIN_PATH || path.join(uxplayWorkdir, 'lib', 'gstreamer-1.0');
const traceSharedTexture =
  process.env.UXPLAY_TRACE_SHARED_TEXTURE === '1' ||
  process.env.UXPLAY_TRACE_SHARED_TEXTURE === 'true';
const sendTimeoutMs = Number.parseInt(process.env.UXPLAY_SEND_TIMEOUT_MS || '1200', 10) || 0;
const uxplayControlTimeoutMs = Number.parseInt(process.env.UXPLAY_CONTROL_TIMEOUT_MS || '3000', 10) || 3000;
const mirrorSessionIdleMs = Number.parseInt(process.env.UXPLAY_MIRROR_IDLE_MS || '3000', 10) || 3000;
const uxplayPinStoreFileName = 'uxplay-pin-state.json';
const controlSessionToken = crypto.randomBytes(24).toString('hex');
const serverPortLogPattern = /Initialized server socket\(s\) on port (\d{1,5})/;

let win;
let bridge;
let bridgeReader;
let bridgeReady = false;
let uxplayHttpPort = null;
let uxplayStderrBuffer = '';
let currentPin = null;
let pinUpdateInFlight = false;
let mirrorAudioEnabled = null;
let mirrorAudioUpdateInFlight = false;
let mirrorSessionActive = false;
let mirrorSessionIdleTimer = null;
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
  win.webContents.send('uxplay-control:status', {
    ready: bridgeReady,
    port: uxplayHttpPort,
    pin: currentPin,
    rotating: pinUpdateInFlight,
    mirrorAudioEnabled,
    muted: typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null,
    audioUpdating: mirrorAudioUpdateInFlight,
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
    mirrorSessionActive = false;
  }, mirrorSessionIdleMs);
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
  if (mirrorSessionActive) {
    return;
  }
  mirrorSessionActive = true;
  try {
    const updated = await rotatePinRandom('auto');
    console.log(`[pin-control] auto rotated pin to ${updated.pin} on mirror session start`);
  } catch (error) {
    console.error(`[pin-control] auto pin rotation failed: ${error?.message || 'unknown error'}`);
    if (shouldRetryAutoPinUpdate(error)) {
      mirrorSessionActive = false;
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

function setupIpcHandlers() {
  ipcMain.handle('uxplay-control:get-session', (event) => {
    try {
      ensureTrustedSender(event);
      return {
        ok: true,
        token: controlSessionToken,
        port: uxplayHttpPort,
        ready: bridgeReady,
        pin: currentPin,
        rotating: pinUpdateInFlight,
        mirrorAudioEnabled,
        muted: typeof mirrorAudioEnabled === 'boolean' ? !mirrorAudioEnabled : null,
        audioUpdating: mirrorAudioUpdateInFlight,
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
}

function createWindow() {
  win = new BrowserWindow({
    width: 1280,
    height: 800,
    backgroundColor: '#101214',
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
    },
  });

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
  if (frameSending || !pendingFrame || !win || win.isDestroyed() || !bridgeReady) {
    return;
  }

  frameSending = true;

  while (pendingFrame && win && !win.isDestroyed()) {
    const frame = pendingFrame;
    pendingFrame = null;
    let sentToRenderer = false;

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
    '-logfile',
    ...extraArgs,
  ];

  const childEnv = {
    ...process.env,
    GST_PLUGIN_PATH: gstreamerPluginPath,
    PATH: `${uxplayWorkdir};${gstreamerBin};${process.env.PATH || ''}`,
  };

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
    mirrorSessionActive = false;
    if (mirrorSessionIdleTimer) {
      clearTimeout(mirrorSessionIdleTimer);
      mirrorSessionIdleTimer = null;
    }
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
    framesReceived += 1;
    touchMirrorSessionActivity();
    void triggerAutoPinRotationOnMirrorStart();
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

app.whenReady().then(() => {
  restorePersistedPin();
  setupIpcHandlers();
  createWindow();
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
  app.quit();
});

app.on('before-quit', () => {
  if (bridge && bridge.stdin.writable) {
    bridge.stdin.write('STOP\n');
  }
  if (mirrorSessionIdleTimer) {
    clearTimeout(mirrorSessionIdleTimer);
    mirrorSessionIdleTimer = null;
  }

  releasePendingFrame(pendingFrame, { notifyRelease: true });
  pendingFrame = null;
});
