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
