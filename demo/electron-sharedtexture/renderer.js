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
const context = canvas.getContext('2d', { alpha: false, desynchronized: true });
let probeFramesRemaining = 5;

function resizeCanvas(width, height) {
  if (canvas.width === width && canvas.height === height) {
    return;
  }

  canvas.width = width;
  canvas.height = height;
}

async function drawFrame(frame, width, height) {
  if (typeof createImageBitmap === 'function') {
    const bitmap = await createImageBitmap(frame);
    try {
      context.drawImage(bitmap, 0, 0, width, height);
      return;
    } finally {
      bitmap.close();
    }
  }

  context.drawImage(frame, 0, 0, width, height);
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

sharedTexture.setSharedTextureReceiver(async ({ importedSharedTexture }, info) => {
  let frame;

  try {
    frame = importedSharedTexture.getVideoFrame();
    resizeCanvas(info.width, info.height);
    await drawFrame(frame, info.width, info.height);
    await probeFrame(frame);

    statusBadge.textContent = 'Texture received';
    stateValue.textContent = 'receiving';
    sizeValue.textContent = `${info.width} x ${info.height}`;
    frameValue.textContent = String(info.frameId ?? '-');
    timestampValue.textContent = `${info.timestampUs ?? '-'} us`;
    textureValue.textContent = importedSharedTexture.textureId;
    if (probeValue.textContent === '-') {
      probeValue.textContent = `fmt=${frame.format || 'unknown'}`;
    }
  } catch (error) {
    console.error('Shared texture render failed:', error);
    statusBadge.textContent = 'Render failed';
    stateValue.textContent = error.message;
  } finally {
    if (frame) {
      frame.close();
    }
    importedSharedTexture.release();
  }
});
