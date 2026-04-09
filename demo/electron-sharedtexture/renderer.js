const { sharedTexture } = require('electron');

if (!sharedTexture) {
  throw new Error('sharedTexture is unavailable in this renderer.');
}

const canvas = document.getElementById('videoCanvas');
const statusBadge = document.getElementById('statusBadge');
const stateValue = document.getElementById('stateValue');
const sizeValue = document.getElementById('sizeValue');
const frameValue = document.getElementById('frameValue');
const timestampValue = document.getElementById('timestampValue');
const textureValue = document.getElementById('textureValue');
const probeValue = document.getElementById('probeValue');
const context = canvas.getContext('2d', { alpha: false });
const query =
  typeof window !== 'undefined' ? new URLSearchParams(window.location.search) : new URLSearchParams();
const enableProbe = query.get('probe') === '1';
const renderTimeoutMs = Number.parseInt(query.get('renderTimeoutMs') || '250', 10) || 0;
let probeFramesRemaining = enableProbe ? 5 : 0;
let drawInFlight = false;
let pendingPacket = null;

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
      textureValue.textContent = importedSharedTexture.textureId;
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

window.addEventListener('beforeunload', () => {
  releasePacket(pendingPacket);
  pendingPacket = null;
});
