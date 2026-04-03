# Electron 40 + GStreamer SharedTexture Demo

This demo shows the smallest end-to-end path:

`GStreamer appsink(D3D11Memory) -> NT handle -> Electron 40 sharedTexture -> renderer canvas`

## Layout

- `native/`: Win32 bridge executable, `gst_texture_bridge.exe`
- `main.js`: Electron main process, imports and forwards shared textures
- `renderer.js`: Renderer-side receiver that draws `VideoFrame` objects

## Requirements

- Windows
- Electron 40.x
- MSYS2 MinGW64 GStreamer, default location `D:\msys64\mingw64`
- `cmake`
- `ninja`

## Run

```powershell
cd demo/electron-sharedtexture
npm install
npm run build:native
npm start
```

## Use Your Own Pipeline

The default source is `videotestsrc`. To plug in a real decode path, keep the output as `BGRA + GstD3D11Memory + appsink name=sink`:

```powershell
$env:GST_SHARED_TEXTURE_PIPELINE='filesrc location=sample.mp4 ! decodebin ! videoconvert ! video/x-raw,format=BGRA ! d3d11upload ! video/x-raw(memory:D3D11Memory),format=BGRA ! appsink name=sink sync=false'
npm start
```

## Notes

- The demo uses a simple "one shared handle per frame" model so the Electron 40 `sharedTexture` path is easy to inspect.
- The bridge first tries to export an NT handle directly from `GstD3D11Memory`. If upstream allocated a non-shareable texture, it falls back to copying into a bridge-owned shareable D3D11 texture before duplicating the handle into Electron.
- `allReferencesReleased` notifies the bridge when it can unref the matching `GstSample`.
