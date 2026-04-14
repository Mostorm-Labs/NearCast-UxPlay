# Electron 40 + UxPlay SharedTexture Demo

This demo shows the smallest end-to-end path:

`UxPlay video pipeline -> D3D11 shared texture -> Electron 40 sharedTexture -> renderer canvas`

## Layout

- `main.js`: Electron main process, launches `uxplay.exe -stpid`, imports and forwards shared textures
- `renderer.js`: Renderer-side receiver that draws `VideoFrame` objects
- `native/`: original standalone bridge prototype kept as reference

## Requirements

- Windows
- Electron 40.x
- Built `UxPlay` binary, default location `..\..\build\uxplay.exe`
- MSYS2 MinGW64 runtime/GStreamer, default location `D:\msys64\mingw64`

## Run

```powershell
cd demo/electron-sharedtexture
npm install
npm start
```

Then start AirPlay mirroring from an Apple device to the server name `UxPlay SharedTexture` or the value of `UXPLAY_SERVER_NAME`.

## Configure UxPlay

By default the demo launches:

```powershell
$env:UXPLAY_EXE='D:\workspace\UxPlay\build\uxplay.exe'
$env:UXPLAY_SERVER_NAME='UxPlay SharedTexture'
$env:UXPLAY_ARGS='-fs -bt709'
$env:UXPLAY_TRACE_SHARED_TEXTURE='1'
npm start
```

`UXPLAY_ARGS` is appended after `-stpid <electronPid> -stdoutlog 0`, so avoid passing another `-stdoutlog` unless you want to change the protocol setup. If you need logs while keeping stdout clean for `READY/FRAME`, prefer adding `-logfile <path>` in `UXPLAY_ARGS`. Set `UXPLAY_TRACE_SHARED_TEXTURE=1` to print Electron-side frame/import/send/release counters.

## Notes

- `uxplay.exe` is started with `-stpid <ElectronPID>` so decoded frames are exported from UxPlay's GStreamer renderer.
- In `-stpid` mode, UxPlay runs export-only video output for stability; the local UxPlay video window is intentionally disabled.
- `READY` and `FRAME\t...` records are read from UxPlay stdout. `allReferencesReleased` sends `RELEASE\t<frameId>` back to UxPlay stdin.
- If `GstD3D11Memory` cannot expose a shareable NT handle directly, UxPlay falls back to copying into a bridge-owned shareable texture before exporting it.

## PIN Control API Integration

- The sidebar shows the current PIN in a read-only field; clicking `Randomize PIN` generates and applies a new random 4-digit PIN.
- On each successful mirroring session start (first received shared-texture frame), the demo automatically rotates to a new random PIN via `PUT /api/pin`.
- The demo parses UxPlay logs for `Initialized server socket(s) on port <port>` (from `lib/httpd.c`) to discover the control port automatically.
- Main process enforces permission checks (trusted renderer + per-session control token) and returns structured error codes for permission, network, upstream, and state failures.
- On successful update, the PIN is persisted in encrypted form via Electron `safeStorage` into `app.getPath('userData')/uxplay-pin-state.json`.
- Control request timeout can be tuned with `UXPLAY_CONTROL_TIMEOUT_MS` (default `3000` ms).
- Mirror session idle detection timeout can be tuned with `UXPLAY_MIRROR_IDLE_MS` (default `3000` ms).

## Mirror Audio Mute API Integration

- The sidebar provides a `Mute` / `Unmute` toggle that calls `PUT /api/audio` with `{"enabled": false|true}`.
- At startup, when UxPlay control port is discovered, the demo requests `GET /api/audio` to sync the current mirror-audio state.
- The main process returns `mirrorAudioEnabled`, `muted`, and `audioUpdating` in `uxplay-control:get-session` / status push so renderer state stays authoritative.
- `uxplay-control:set-muted` uses the same trusted-renderer + control-token gate as PIN APIs.

## Stop Casting API Integration

- The sidebar provides a `Stop Casting` button that calls `POST /api/stop`.
- The main process exposes `uxplay-control:stop-casting` and reuses the trusted-renderer + per-session control token checks used by PIN/audio controls.
- `uxplay-control:get-session` / status push now include `stopUpdating` so renderer button state and feedback remain authoritative.
