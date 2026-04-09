const { app, BrowserWindow, sharedTexture } = require('electron');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
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

let win;
let bridge;
let bridgeReader;
let bridgeReady = false;
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
    '-stdoutlog',
    '0',
    '-d',
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
      console.log(`UxPlay shared texture export is ready. AirPlay server name: ${uxplayServerName}`);
      void flushFrameQueue();
      return;
    }

    const [type, ...rest] = line.split('\t');
    if (type !== 'FRAME' || rest.length < 5) {
      console.log(`[uxplay] ${line}`);
      return;
    }

    const [frameIdText, widthText, heightText, timestampText, handleHex] = rest;
    const frameId = Number(frameIdText);
    const width = Number(widthText);
    const height = Number(heightText);
    const timestampUs = Number(timestampText);
    framesReceived += 1;
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
    process.stderr.write(chunk);
  });
}

app.whenReady().then(() => {
  createWindow();
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

  releasePendingFrame(pendingFrame, { notifyRelease: true });
  pendingFrame = null;
});
