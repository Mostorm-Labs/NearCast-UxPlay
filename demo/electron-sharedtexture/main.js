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

const bridgeExecutable = path.join(
  __dirname,
  'native',
  'build',
  'gst_texture_bridge.exe',
);

const defaultPipeline =
  process.env.GST_SHARED_TEXTURE_PIPELINE ||
  'videotestsrc is-live=true pattern=smpte ! ' +
    'video/x-raw,format=BGRA,width=1280,height=720,framerate=30/1 ! ' +
    'queue max-size-buffers=2 leaky=downstream ! ' +
    'd3d11upload ! ' +
    'video/x-raw(memory:D3D11Memory),format=BGRA ! ' +
    'appsink name=sink sync=false';
const msysRoot = process.env.MSYS64_ROOT || 'D:\\msys64';
const gstreamerBin = path.join(msysRoot, 'mingw64', 'bin');

let win;
let bridge;
let bridgeReader;
let bridgeReady = false;
let frameSending = false;
let pendingFrame = null;

if (!app || !BrowserWindow || !sharedTexture) {
  throw new Error('Electron sharedTexture API is unavailable. Use Electron 40+ and ensure ELECTRON_RUN_AS_NODE is not set.');
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

function releasePendingFrame(frame) {
  if (!frame) {
    return;
  }

  try {
    frame.importedSharedTexture.release();
  } catch {
    // Ignore double release during shutdown.
  }
}

function queueFrame(frame) {
  if (pendingFrame) {
    releasePendingFrame(pendingFrame);
  }

  pendingFrame = frame;
  void flushFrameQueue();
}

async function flushFrameQueue() {
  if (frameSending || !pendingFrame || !win || win.isDestroyed() || !bridgeReady) {
    return;
  }

  frameSending = true;

  while (pendingFrame && win && !win.isDestroyed()) {
    const frame = pendingFrame;
    pendingFrame = null;

    try {
      await sharedTexture.sendSharedTexture(
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
    } catch (error) {
      console.error('sendSharedTexture failed:', error);
    } finally {
      releasePendingFrame(frame);
    }
  }

  frameSending = false;
}

function startBridge() {
  if (!fs.existsSync(bridgeExecutable)) {
    throw new Error(`Native bridge not found: ${bridgeExecutable}. Run npm run build:native first.`);
  }

  const childEnv = {
    ...process.env,
    PATH: `${gstreamerBin};${process.env.PATH || ''}`,
  };

  bridge = spawn(
    bridgeExecutable,
    ['--electron-pid', String(process.pid), '--pipeline', defaultPipeline],
    {
      cwd: __dirname,
      env: childEnv,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    },
  );

  bridge.on('error', (error) => {
    console.error('Failed to launch bridge:', error);
  });

  bridge.on('exit', (code, signal) => {
    console.error(`Bridge exited code=${code} signal=${signal}`);
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
      void flushFrameQueue();
      return;
    }

    const [type, ...rest] = line.split('\t');
    if (type !== 'FRAME' || rest.length < 5) {
      return;
    }

    const [frameIdText, widthText, heightText, timestampText, handleHex] = rest;
    const frameId = Number(frameIdText);
    const width = Number(widthText);
    const height = Number(heightText);
    const timestampUs = Number(timestampText);

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
          if (bridge && bridge.stdin.writable) {
            bridge.stdin.write(`RELEASE\t${frameId}\n`);
          }
        },
      });
    } catch (error) {
      console.error('importSharedTexture failed:', error);
      if (bridge && bridge.stdin.writable) {
        bridge.stdin.write(`RELEASE\t${frameId}\n`);
      }
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

  releasePendingFrame(pendingFrame);
  pendingFrame = null;
});
