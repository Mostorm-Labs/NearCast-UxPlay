# electron-sharedtexture 后台化改造计划

## 目标

将 `demo/electron-sharedtexture` 从当前的可视化 demo 改造成后台常驻应用：

- 应用启动后在后台运行，不默认显示投屏窗口。
- 只有发生投屏时才显示投屏窗口。
- 需要输入 PIN 码时弹出独立的小 PIN 窗口。
- 外部程序可以通过 WebSocket 控制应用并接收状态事件。
- 保留现有 UxPlay sharedTexture 渲染链路、PIN 控制、静音控制、停止投屏等能力。

## 当前基础

当前 `demo/electron-sharedtexture/main.js` 已具备以下能力：

- 启动 `uxplay.exe` 子进程。
- 通过 `sharedTexture` 接收 UxPlay 导出的 D3D11 纹理并发送给 renderer。
- 通过 UxPlay HTTP 控制端口更新 PIN、控制镜像音频、停止投屏。
- 解析 UxPlay 日志发现 HTTP 控制端口。
- 维护 `castingActive`、`bridgeReady`、`currentPin`、`mirrorAudioEnabled` 等运行状态。
- 通过 Electron IPC 给现有 renderer 提供控制能力。
- 启动 UxPlay 时可传入 `-ws-enable` 和 `-ws-port`。

## 必须完成

### 1. 后台应用生命周期

- 启动后不显示主投屏窗口。
- 将主投屏窗口改为预创建但隐藏，例如 `show: false`。
- 投屏开始时调用 `show()`，投屏结束时调用 `hide()`。
- 修改 `window-all-closed` 行为，避免所有窗口隐藏或关闭后直接退出应用。
- 增加 `app.requestSingleInstanceLock()`，避免多个后台实例同时启动多个 AirPlay 服务。
- 增加明确退出路径，例如托盘菜单中的“退出”。

### 2. 投屏窗口显示与隐藏

- 将窗口显示/隐藏逻辑接入 `setCastingActive(nextActive, reason, options)`。
- `castingActive=true` 时：
  - 显示投屏窗口。
  - 按配置决定是否全屏。
  - 触发 `flushFrameQueue()`。
- `castingActive=false` 时：
  - 清理 pending frame。
  - 停止投屏画面展示。
  - 隐藏投屏窗口。
- 保持主投屏窗口预创建，避免首帧到达时才创建窗口导致 sharedTexture renderer 不可用或首帧延迟。

### 3. 独立 PIN 小窗口

- 新增独立 `BrowserWindow`，例如 `pinWindow`。
- 新增 `pin.html` 与 `pin-renderer.js`。
- PIN 小窗只负责展示 PIN，不承载投屏画面和调试控制面板。
- PIN 变化时由 main process 同步推送给 PIN 小窗。
- PIN 认证完成、投屏开始或超时后自动隐藏 PIN 小窗。
- 小窗建议配置：
  - 小尺寸。
  - `alwaysOnTop: true`。
  - 不显示菜单栏。
  - 可选不抢焦点。

### 4. PIN 请求事件来源

当前代码能生成和更新 PIN，但还缺少“设备正在请求输入 PIN”的明确事件。需要选择一种方案：

- 优先方案：修改 UxPlay，在认证流程中输出明确事件，例如 `PIN_REQUIRED`、`PIN_ACCEPTED`、`PIN_REJECTED`。
- 次选方案：扩展 UxPlay WebSocket 事件，把 PIN 请求状态发给 Electron。
- 临时方案：解析 UxPlay 日志中与 PIN/认证相关的文本，但稳定性较弱。

Electron main process 收到 PIN 请求事件后：

- 确保当前 PIN 可用。
- 显示 PIN 小窗。
- 广播 WebSocket 状态事件 `pin.required`。

### 5. UI 拆分

将当前 `index.html` 和 `renderer.js` 的职责拆开：

- `cast.html` / `cast-renderer.js`
  - 只负责投屏画面。
  - 接收 sharedTexture `VideoFrame`。
  - 展示投屏状态。
- `pin.html` / `pin-renderer.js`
  - 只负责 PIN 展示。
- 可选 `control.html` / `control-renderer.js`
  - 内部调试面板。
  - 提供刷新 PIN、停止投屏、静音、窗口控制等按钮。

后台正式模式中，投屏窗口不再显示当前 demo 的侧边栏控制区。

### 6. Electron 自身 WebSocket 控制服务

新增一个 Electron main process 内部 WebSocket 服务，和 UxPlay 自身 WebSocket 区分开。

- 新增依赖：`ws`。
- 默认监听 `127.0.0.1`。
- 默认端口建议独立配置，例如 `UXPLAY_APP_WS_PORT=7010`。
- 通过环境变量或配置控制是否允许局域网访问。
- 增加认证 token，避免未授权程序控制应用。

建议配置项：

- `UXPLAY_APP_WS_ENABLE`
- `UXPLAY_APP_WS_HOST`
- `UXPLAY_APP_WS_PORT`
- `UXPLAY_APP_WS_TOKEN`

### 7. WebSocket 控制命令

建议支持以下命令：

- `getStatus`
- `rotatePin`
- `setPin`
- `setMuted`
- `stopCasting`
- `showCastWindow`
- `hideCastWindow`
- `setFullscreen`
- `showPinWindow`
- `hidePinWindow`
- `restartUxPlay`
- `quitApp`

### 8. WebSocket 状态事件

建议广播以下事件：

- `app.ready`
- `uxplay.ready`
- `uxplay.exited`
- `control.portChanged`
- `pin.changed`
- `pin.required`
- `pin.accepted`
- `pin.rejected`
- `pin.hidden`
- `casting.started`
- `casting.stopped`
- `casting.frameStats`
- `audio.changed`
- `window.changed`
- `error`

### 9. 统一状态模型

新增统一状态函数，例如 `getAppStatus()`，集中返回：

- app 是否 ready。
- UxPlay 是否 ready。
- UxPlay HTTP 控制端口。
- Electron WebSocket 地址。
- UxPlay WebSocket 地址。
- 是否正在投屏。
- 当前 PIN。
- PIN 是否正在更新。
- 镜像音频状态。
- 投屏窗口状态。
- PIN 小窗状态。
- 最近错误。

现有 `broadcastControlStatus()` 应改造成同时推送到：

- 投屏窗口 renderer。
- PIN 小窗 renderer。
- 调试控制窗口 renderer。
- Electron WebSocket clients。

## 可能需要完成

### 1. 托盘菜单

- 显示当前运行状态。
- 显示/隐藏投屏窗口。
- 显示 PIN。
- 刷新 PIN。
- 停止投屏。
- 打开调试控制面板。
- 退出应用。

### 2. 开机自启

- 使用 `app.setLoginItemSettings()`。
- 默认不强制开启。
- 通过配置或托盘菜单控制。

### 3. 多屏策略

- 配置投屏窗口默认显示在哪个显示器。
- 支持 WebSocket 指定 display id。
- 显示器断开后自动回退到主屏。
- 可配置是否默认全屏。

### 4. 窗口行为配置

- 投屏窗口是否置顶。
- 投屏窗口是否显示在任务栏。
- 投屏结束后是隐藏窗口还是保留黑屏。
- PIN 小窗是否抢焦点。
- PIN 小窗自动隐藏超时时间。

### 5. 安全策略

- WebSocket 默认只监听本机。
- 局域网访问必须显式开启。
- token 支持从环境变量或配置文件读取。
- 避免在日志里长期打印 PIN。
- 保留 `safeStorage` 加密保存 PIN 的机制。

### 6. 异常恢复

- UxPlay 子进程崩溃后自动重启。
- WebSocket 端口占用时给出明确错误。
- renderer 崩溃时重建窗口。
- sharedTexture import/send 连续失败时重置会话。
- 投屏结束时确保释放 pending texture。

### 7. 打包调整

- 增加托盘图标资源。
- 更新 exe 名称和产品名。
- 打包时包含新增的 HTML、JS、图标文件。
- 确认 `ws` 依赖被打包。
- 如开放局域网端口，补充 Windows 防火墙说明。

## 建议实施阶段

### 阶段一：后台常驻和投屏窗口按需显示

- 改造 `createWindow()`，使投屏窗口隐藏启动。
- 修改 `window-all-closed`，让应用保持后台运行。
- 在 `setCastingActive()` 中控制投屏窗口显示/隐藏。
- 加单实例锁。
- 加最小可用托盘退出菜单。

验收：

- 启动应用后没有投屏窗口。
- iPhone 或其他 AirPlay 设备开始投屏后窗口自动显示。
- 停止投屏后窗口自动隐藏。
- 应用没有退出，AirPlay 服务仍可再次被发现。

### 阶段二：PIN 小窗

- 新增 `pin.html` 和 `pin-renderer.js`。
- 新增 `createPinWindow()`、`showPinWindow()`、`hidePinWindow()`。
- PIN 更新时同步小窗内容。
- 先用手动触发或临时事件验证小窗弹出流程。

验收：

- 可以通过内部调用显示 PIN 小窗。
- PIN 变化后小窗内容同步变化。
- 投屏开始或超时后小窗隐藏。

### 阶段三：PIN 请求事件接入

- 修改 UxPlay 或事件桥，输出明确 PIN 请求状态。
- Electron main process 解析并转成应用状态事件。
- PIN 请求时自动弹出 PIN 小窗。

验收：

- 设备请求 PIN 时，小窗自动出现。
- 认证完成后，小窗自动隐藏。
- PIN 错误或认证失败时有状态事件。

### 阶段四：Electron WebSocket 控制服务

- 安装 `ws`。
- 新增 WebSocket server。
- 实现 token 校验。
- 实现 `getStatus`、`rotatePin`、`setMuted`、`stopCasting`。
- 广播核心状态事件。

验收：

- 外部 WebSocket 客户端能连接并鉴权。
- 外部能获取状态、刷新 PIN、停止投屏、切换静音。
- 投屏开始/结束、PIN 更新等事件能被外部收到。

### 阶段五：窗口和高级控制

- 实现 `showCastWindow`、`hideCastWindow`、`setFullscreen`。
- 实现 `showPinWindow`、`hidePinWindow`。
- 可选支持多屏 display id。
- 可选支持 UxPlay 重启。

验收：

- 外部可以控制投屏窗口显示、隐藏、全屏。
- 外部可以控制 PIN 小窗显示、隐藏。
- 错误命令不会导致主进程异常。

### 阶段六：打包和稳定性

- 更新打包脚本，包含新增资源和依赖。
- 验证打包后 UxPlay runtime、GStreamer、Electron 资源均可用。
- 验证后台启动、投屏、PIN 小窗、WebSocket 控制完整流程。
- 增加异常恢复和日志。

验收：

- 打包产物可在干净 Windows 环境运行。
- 应用可后台常驻。
- 投屏流程稳定。
- WebSocket 控制可用。
- 托盘退出可用。

## 推荐优先级

1. 后台常驻和投屏窗口按需显示。
2. 独立 PIN 小窗。
3. 明确 PIN 请求事件。
4. Electron WebSocket 控制服务。
5. 托盘菜单和配置项。
6. 打包、稳定性和异常恢复。

