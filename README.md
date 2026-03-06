# 编译指南

本仓库以 [UxPlay v1.72.1](https://github.com/FDH2/UxPlay/tree/v1.72.1) 源码为基础，并在此之上补充了 Windows 所需的依赖与自动化脚本。下文依据 `build_uxplay-windows.yml` 的流程整理，帮助你在本地编译该工程。除特别说明外，所有命令默认在 PowerShell 中执行。

## 1. 环境要求

- Windows 10/11 x64，具备管理员权限。
- [MSYS2](https://www.msys2.org/) 并预装 `mingw64` 工具链。
- Git、CMake、Ninja（MSYS2 安装包会顺带提供这些工具）。

## 2. 步骤总览

1. 使用仓库根目录自带的 `bonjoursdksetup.exe` 安装 Bonjour SDK，获得 `dns_sd.h` 与 `dnssd.lib`。
2. 使用 MSYS2 安装 UxPlay 所需的 MinGW 依赖。

以下为可直接执行的细化指引。

## 3. 详细指引

### 3.1 安装仓库内的 Bonjour SDK

仓库根目录已经包含 `bonjoursdksetup.exe`，直接运行即可完成安装（需要管理员权限）。

```powershell
Start-Process -FilePath (Join-Path (Get-Location) "bonjoursdksetup.exe") -ArgumentList '/quiet' -Wait
```

安装结束后，确认以下文件存在：

- `C:\Program Files\Bonjour SDK\Include\dns_sd.h`
- `C:\Program Files\Bonjour SDK\Lib\x64\dnssd.lib`

这两个文件会在后续 CMake 配置时被自动发现（或通过 `CMAKE_PREFIX_PATH` 手动指定）。

### 3.2 安装 MSYS2 依赖

本地构建直接使用 pacman 即可。首次打开 `MSYS2 MSYS` shell 后执行系统更新：

```bash
pacman -Syu
```

更新完成并重启 shell 后，切换到 `MSYS2 MinGW 64-bit`（或 `UCRT64`）shell，安装构建 UxPlay 所需的工具链与运行时：

```bash
pacman -S --needed \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-libplist \
  mingw-w64-x86_64-json-glib \
  mingw-w64-x86_64-gstreamer \
  mingw-w64-x86_64-gst-plugins-base \
  mingw-w64-x86_64-gst-libav \
  mingw-w64-x86_64-gst-plugins-good \
  mingw-w64-x86_64-gst-plugins-bad
```

上述命令会安装 `cmake`、`ninja`、`gstreamer` 以及 GitHub Actions 同款的插件集合，确保本地与 CI 行为一致。

### 3.3 编译 UxPlay 主程序

此步骤基于当前仓库中的 UxPlay 源码，无需额外 `git clone`。在 `MSYS2 MinGW 64-bit`（或 `UCRT64`）shell 中，先切换到仓库根目录后执行：

```bash
mkdir -p build && cd build
cmake .. -G Ninja
ninja

mkdir -p artifact/lib
cd artifact
cp -r /mingw64/lib/gstreamer-1.0 lib/gstreamer-1.0
cp -r /mingw64/bin bin
cp ../uxplay.exe bin/
```

## 4. 故障与排查

- **Bonjour SDK 安装程序缺失或损坏**：确认仓库根目录存在 `bonjoursdksetup.exe`。若文件不可用，可改用原工作流中的 mDNSResponder 源码编译方式获取 `dns_sd.h`/`dnssd.lib`。
- **MSYS2 包冲突**：运行 `pacman -Syu` 进行完整更新，再重新安装依赖；必要时清理 `C:\msys64\var\cache\pacman\pkg` 后重试。
- **裁剪后运行缺少 DLL**：先在未裁剪的 `build/artifact` 目录验证 UxPlay 可正常启动，再逐步收紧 `dll-libs-list.txt`，每次变更都重新测试。
- **CMake 找不到 Bonjour 库**：将 `C:\Program Files\Bonjour SDK` 追加到 `CMAKE_PREFIX_PATH` 或设置 `DNSSD_ROOT_DIR`，例如 `cmake .. -G Ninja -DDNSSD_ROOT_DIR="C:/Program Files/Bonjour SDK"`。
