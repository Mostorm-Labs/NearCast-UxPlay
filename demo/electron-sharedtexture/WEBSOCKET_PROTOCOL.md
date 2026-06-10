# UxPlay / Electron Demo WebSocket 协议说明

本文档描述 Electron 应用控制口对外暴露的 AXTP WebSocket JSON 协议，以及它与 UxPlay 内部控制口之间的适配关系。

外部控制口使用 `AXTP-WS-JSON`：所有 WebSocket text frame 都必须是 JSON，并使用 `sid/op/d` 外层结构。旧轻量协议
`type/id/op/data/ok/error` 不再转换或兼容。

> 说明：`cast.*` 业务方法与事件当前按 `D:\workspace\axtp\docs\flows\cast-reciever-uxplay.md` 作为候选协议记录。进入 registry/generated 前，不应把它们作为 adopted SDK 合同发布。

## 1. 链路

```text
Launcher / UI / 外部控制端
    | ws://127.0.0.1:7010/
    v
Electron 应用控制口（Cast Receiver AXTP Adapter）
    | ws://127.0.0.1:7001/
    v
UxPlay WebSocket control server（内部 backend adapter）
```

Electron 应用控制口是 AXTP Logical Server；Launcher、UI 或外部控制端是 Logical Client。WebSocket 建立后由服务端先发送 `Hello`。

Electron 启动 UxPlay 时会传入：

```text
-ws-enable -ws-port <UXPLAY_WS_PORT>
```

并通过环境变量传入 UxPlay 内部控制 token：

```text
UXPLAY_CONTROL_TOKEN=<48 hex chars>
```

UxPlay `7001` 控制口只作为内部实现细节，不直接作为公共 AXTP 协议暴露。

## 2. AXTP 消息模型

### Envelope

```json
{
  "sid": "12345678",
  "op": 7,
  "d": {}
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `sid` | string | RPC session id。握手前为空字符串；`Identified` 后使用服务端分配的 8 位 hex id。 |
| `op` | number | OpCode，标识消息类型。 |
| `d` | object | 当前消息内容。请求参数、响应结果和事件数据都放在这里。 |

### OpCode

| op | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| `0` | `Hello` | Server -> Client | 服务端建立连接后主动宣布 AXTP/RPC version 和认证要求。 |
| `2` | `Identify` | Client -> Server | 客户端提交 RPC version、身份、认证和订阅意图。 |
| `3` | `Identified` | Server -> Client | 服务端确认 session ready，并返回 `sid`。 |
| `6` | `Event` | Server -> Client | 服务端广播状态、会话、PIN、音频、窗口和错误事件。 |
| `7` | `Request` | Client -> Server | 客户端调用业务 method。 |
| `8` | `RequestResponse` | Server -> Client | 服务端返回请求结果。 |

`HelloAck`、legacy `auth` method、`Bye/ByeAck` 只作为迁移线索保留；新外部控制口以 AXTP RPC session 握手为准。

### Hello `op=0`

```json
{
  "sid": "",
  "op": 0,
  "d": {
    "axtpVersion": "1.0.0",
    "rpcVersion": 1
  }
}
```

如果需要认证，服务端可在 `d` 中携带 challenge、salt 或认证策略字段。具体 token/HMAC/scopes 仍需在 Stage 20 协议草案中确认。

### Identify `op=2`

```json
{
  "sid": "",
  "op": 2,
  "d": {
    "rpcVersion": 1,
    "eventMasks": "",
    "authentication": {
      "type": "token",
      "token": "<token>"
    }
  }
}
```

本机控制是否可默认 no-auth、LAN 控制是否必须 token/HMAC、Origin 白名单和 token 轮换策略，仍需产品与协议草案确认。

### Identified `op=3`

```json
{
  "sid": "12345678",
  "op": 3,
  "d": {
    "negotiatedRpcVersion": 1
  }
}
```

`Identified` 前收到业务 `Request` 时，服务端应拒绝处理或关闭连接。

### Request `op=7`

```json
{
  "sid": "12345678",
  "op": 7,
  "d": {
    "id": 1,
    "method": "cast.getStatus",
    "params": {}
  }
}
```

### RequestResponse `op=8`

成功：

```json
{
  "sid": "12345678",
  "op": 8,
  "d": {
    "id": 1,
    "status": {
      "ok": true,
      "code": 0
    },
    "result": {
      "state": "ready"
    }
  }
}
```

失败：

```json
{
  "sid": "12345678",
  "op": 8,
  "d": {
    "id": 1,
    "status": {
      "ok": false,
      "code": "INVALID_PARAMS",
      "message": "invalid request params"
    }
  }
}
```

### Event `op=6`

```json
{
  "sid": "12345678",
  "op": 6,
  "d": {
    "event": "cast.sessionStarted",
    "intent": 1,
    "data": {
      "sessionId": "cast-001",
      "protocol": "airplay"
    }
  }
}
```

## 3. 交互流程

```text
1. Launcher 启动 Electron/UxPlay 接收端 runtime。
2. Electron 启动 UxPlay backend，并连接内部 ws://127.0.0.1:7001/。
3. 控制端连接外部 ws://127.0.0.1:7010/。
4. 服务端发送 Hello(op=0, sid="")。
5. 客户端发送 Identify(op=2)，提交 rpcVersion、认证和订阅意图。
6. 服务端返回 Identified(op=3)，分配 8 位 hex sid。
7. 服务端可广播 cast.runtimeReady、cast.backendReady 等事件。
8. 客户端发送业务 Request(op=7, method="cast.*")。
9. 服务端返回 RequestResponse(op=8)，并在状态变化时广播 Event(op=6)。
10. WebSocket 断开即可表示 session 结束；是否启用应用层 Bye 另行评审。
```

投屏开始的典型序列：

```text
iOS/macOS Source
  -> 通过 mDNS 发现 AirPlay receiver
  -> 向 UxPlay 发起 AirPlay mirroring
UxPlay
  -> 内部事件 pinRequired / mirrorStarted
Electron AXTP Adapter
  -> 归一化为 cast.pinCodeRequired / cast.sessionStarted / cast.statusChanged
Launcher / UI
  -> 展示 PIN toast 和投屏窗口
```

## 4. Electron 应用控制口

- 地址：`ws://127.0.0.1:7010/`
- 角色：AXTP Logical Server / Cast Receiver AXTP Adapter
- Transport profile：`AXTP-WS-JSON`
- WebSocket text frame：直接承载 JSON `{ "sid": string, "op": number, "d": object }`
- `UXPLAY_APP_WS_ENABLE=0|false`：关闭 Electron 应用控制口
- `UXPLAY_APP_WS_PORT=<port>`：修改端口，默认 `7010`
- `UXPLAY_APP_WS_HOST=<host>`：设置监听 host
- `UXPLAY_APP_WS_ALLOW_LAN=1|true`：允许按 `UXPLAY_APP_WS_HOST` 监听，否则强制 `127.0.0.1`
- `UXPLAY_APP_WS_TOKEN=<token>`：候选外部控制 token；具体认证语义进入 `Identify.d.authentication`
- 最大接收 payload：`64 KiB`

### 候选方法

| Feature | method | params | result |
| --- | --- | --- | --- |
| `cast.status` | `cast.getStatus` | `{}` | 完整投屏状态对象。 |
| `cast.session` | `cast.getSession` | `{}` | 当前投屏会话摘要。 |
| `cast.session` | `cast.stopSession` | `{ "reason"?: string }` | 停止当前投屏结果。 |
| `cast.pinCode` | `cast.getPinCode` | `{}` | PIN 状态；是否允许明文返回需评审。 |
| `cast.pinCode` | `cast.setPinCode` | `{ "pinCode": string }` | PIN 设置结果。 |
| `cast.pinCode` | `cast.rotatePinCode` | `{}` | PIN 轮换结果。 |
| `cast.pinCode` | `cast.showPinCode` | `{}` | PIN 展示状态。 |
| `cast.pinCode` | `cast.hidePinCode` | `{}` | PIN 隐藏状态。 |
| `cast.audio` | `cast.getAudio` | `{}` | 音频状态。 |
| `cast.audio` | `cast.setAudio` | `{ "enabled": boolean }` | mirror audio enabled 应用结果。 |
| `cast.audio` | `cast.setMuted` | `{ "muted": boolean }` | 本地静音应用结果。 |
| `cast.window` | `cast.getWindowState` | `{}` | 投屏窗口状态。 |
| `cast.window` | `cast.showWindow` | `{}` | 显示投屏窗口结果。 |
| `cast.window` | `cast.hideWindow` | `{}` | 隐藏投屏窗口结果。 |
| `cast.window` | `cast.setFullscreen` | `{ "fullscreen": boolean }` | 全屏状态和完整应用状态。 |
| `cast.window` | `cast.setAlwaysOnTop` | `{ "alwaysOnTop": boolean }` | 置顶状态和完整应用状态。 |
| `cast.runtime` | `cast.getDisplayName` | `{}` | `{ "displayName": string }`。 |
| `cast.runtime` | `cast.setDisplayName` | `{ "displayName": string }` | 显示名更新结果。 |
| `cast.runtime` | `cast.getRuntimeStatus` | `{}` | runtime 状态。 |
| `cast.runtime` | `cast.restartRuntime` | `{}` | runtime 重启结果；权限需评审。 |
| `cast.runtime` | `cast.quitRuntime` | `{}` | `{ "message": "quitting" }`；权限需评审。 |
| `cast.backend` | `cast.getBackendStatus` | `{}` | backend 状态。 |
| `cast.backend` | `cast.restartBackend` | `{}` | 当前 backend 重启结果。 |

### 候选事件

Electron 应用控制口会向已完成 `Identified` 的客户端广播事件。

| Feature | event | data |
| --- | --- | --- |
| `cast.status` | `cast.statusChanged` | 完整投屏状态对象。 |
| `cast.status` | `cast.error` | `{ "code": string, "message": string, "details"?: object }`。 |
| `cast.runtime` | `cast.runtimeReady` | runtime 摘要。 |
| `cast.runtime` | `cast.runtimeChanged` | runtime 状态变化。 |
| `cast.runtime` | `cast.displayNameChanged` | `{ "displayName": string, "previousDisplayName"?: string, "reason"?: string }`。 |
| `cast.runtime` | `cast.controlPortChanged` | `{ "port": number }`。 |
| `cast.backend` | `cast.backendReady` | `{ "type": "uxplay", "state": "ready", "controlPort": 7001 }`。 |
| `cast.backend` | `cast.backendExited` | `{ "code": number|null, "signal": string|null, "reason"?: string }`。 |
| `cast.backend` | `cast.backendChanged` | backend 状态变化。 |
| `cast.session` | `cast.sessionStarted` | `{ "sessionId": string, "source": object, "protocol": "airplay" }`。 |
| `cast.session` | `cast.sessionStopped` | `{ "sessionId"?: string, "reason"?: string }`。 |
| `cast.session` | `cast.frameStats` | frame stats 对象。 |
| `cast.pinCode` | `cast.pinCodeRequired` | `{ "pinCode"?: string, "reason"?: string }`；是否允许明文 PIN 需评审。 |
| `cast.pinCode` | `cast.pinCodeAccepted` | `{ "pinCode"?: string, "reason"?: string }`。 |
| `cast.pinCode` | `cast.pinCodeChanged` | `{ "pinCode"?: string, "source"?: string }`。 |
| `cast.pinCode` | `cast.pinCodeHidden` | `{ "pinCode"?: string }`。 |
| `cast.audio` | `cast.audioChanged` | `{ "enabled": boolean, "muted": boolean, "source"?: string }`。 |
| `cast.window` | `cast.windowChanged` | `{ "window": "cast", "action": string, "reason"?: string, "state": object }`。 |

### 状态对象示例

```json
{
  "roles": ["receiver"],
  "activeRole": "receiver",
  "state": "ready",
  "protocols": ["airplay"],
  "runtime": {
    "state": "ready",
    "displayName": "Launcher Cast Receiver",
    "controlPort": 7010
  },
  "backend": {
    "type": "uxplay",
    "state": "ready",
    "controlPort": 7001
  },
  "session": {
    "active": false,
    "sessionId": null
  },
  "pinCode": {
    "required": true,
    "visible": false
  },
  "audio": {
    "enabled": true,
    "muted": false
  },
  "window": {
    "visible": false,
    "fullscreen": false,
    "alwaysOnTop": false
  }
}
```

## 5. Legacy 到 AXTP 候选映射

| Legacy method/event | Candidate AXTP method/event | 说明 |
| --- | --- | --- |
| `HelloAck` | `Identified(op=3)` | 新 AXTP 不再使用 `HelloAck` 作为握手确认。 |
| `auth` | `Identify.d.authentication` / future `auth.*` | 认证放到 handshake 或 auth 草案，不作为 `cast.*` method。 |
| `getStatus` | `cast.getStatus` | 聚合 runtime/backend/session/pin/audio/window 状态。 |
| `status.changed` | `cast.statusChanged` | 整体投屏状态变化。 |
| `error` | `cast.error` | 投屏能力错误。 |
| `app.ready` | `cast.runtimeReady` | 投屏接收端 runtime 已就绪。 |
| `control.portChanged` | `cast.controlPortChanged` | 外部控制口端口变化。 |
| `getServerName` | `cast.getDisplayName` | 获取投屏服务显示名。 |
| `setServerName` / `setUxPlayServerName` | `cast.setDisplayName` | 不把 UxPlay 放进公共 method name。 |
| `quitApp` | `cast.quitRuntime` | 退出 runtime；权限需评审。 |
| `uxplay.ready` | `cast.backendReady` | UxPlay 是 backend type。 |
| `uxplay.exited` | `cast.backendExited` | backend 退出或崩溃。 |
| `restartUxPlay` | `cast.restartBackend` | 重启当前 backend。 |
| `mirrorStarted` / `casting.started` | `cast.sessionStarted` | 投屏会话开始。 |
| `mirrorStopped` / `casting.stopped` | `cast.sessionStopped` | 投屏会话停止。 |
| `casting.frameStats` | `cast.frameStats` | 投屏帧统计。 |
| `stop` / `stopCasting` | `cast.stopSession` | 停止当前投屏。 |
| `getPin` | `cast.getPinCode` | PIN 读取权限需评审。 |
| `setPin` | `cast.setPinCode` | 设置 PIN。 |
| `rotatePin` | `cast.rotatePinCode` | 轮换 PIN。 |
| `showPinWindow` | `cast.showPinCode` | PIN 展示属于 `cast.pinCode`。 |
| `hidePinWindow` | `cast.hidePinCode` | PIN 隐藏属于 `cast.pinCode`。 |
| `pinRequired` / `pin.required` | `cast.pinCodeRequired` | PIN required event。 |
| `pin.accepted` | `cast.pinCodeAccepted` | PIN 被接受。 |
| `pinChanged` / `pin.changed` | `cast.pinCodeChanged` | PIN 变化。 |
| `pin.hidden` | `cast.pinCodeHidden` | PIN 被隐藏。 |
| `getAudio` | `cast.getAudio` | 获取音频状态。 |
| `setAudio` | `cast.setAudio` | 控制 mirror audio enabled。 |
| `setMuted` | `cast.setMuted` | 控制静音。 |
| `audioChanged` / `audio.changed` | `cast.audioChanged` | 音频状态变化。 |
| `showCastWindow` | `cast.showWindow` | 显示投屏窗口。 |
| `hideCastWindow` | `cast.hideWindow` | 隐藏投屏窗口。 |
| `setFullscreen` | `cast.setFullscreen` | 全屏控制。 |
| `setAlwaysOnTop` | `cast.setAlwaysOnTop` | 置顶控制。 |
| `window.changed` | `cast.windowChanged` | 窗口变化。 |

## 6. UxPlay 内部控制口

UxPlay 内部控制口用于 Electron backend adapter 与 UxPlay 进程通信，不作为公共 AXTP 控制协议。

- 地址：`ws://127.0.0.1:7001/`
- CLI：`-ws-enable -ws-port <port>`
- 只监听 `127.0.0.1`
- WebSocket URI 只接受 `/`
- `Origin` 为空时允许；非空时必须包含 `127.0.0.1` 或 `localhost`
- 最大接收 payload：`16 KiB`

### 内部 legacy 操作

| method | params | result |
| --- | --- | --- |
| `auth` | `{ "token": string }` | `{ "message": "authenticated" }` |
| `setPin` | `{ "pin": "1234" }` | `{ "pin": "1234" }` |
| `getPin` | `{}` | `{ "pin": "1234" }` 或 `{ "pin": null }` |
| `setAudio` | `{ "enabled": boolean }` | `{ "mirrorAudio": boolean }` |
| `getAudio` | `{}` | `{ "mirrorAudio": boolean }` |
| `stop` | `{}` | `{ "message": "casting stopped" }` |
| `getStatus` | `{}` | UxPlay 状态对象 |

### 内部 legacy 事件

UxPlay 事件只会广播给已经通过内部 `auth` 的连接。

| event | data |
| --- | --- |
| `mirrorStarted` | `{ "device": string, "model": string, "deviceId": string, "ip": null }` |
| `mirrorStopped` | `{ "reason": string }` 或 `{}` |
| `pinChanged` | `{ "pin": string }` |
| `pinRequired` | `{ "pin": string }` |
| `audioChanged` | `{ "mirrorAudio": boolean }` |

## 7. 候选错误码

| code | 含义 |
| --- | --- |
| `UNAUTHORIZED` | 未认证、认证失败或权限不足。 |
| `INVALID_PARAMS` | JSON 格式、参数类型或参数值不合法。 |
| `INVALID_STATE` | 控制对象暂不可用。 |
| `INVALID_OP` | 未知 opcode 或当前阶段不允许该 opcode。 |
| `CAST_NO_ACTIVE_SESSION` | 没有可停止的活动投屏会话。 |
| `CAST_WINDOW_NOT_AVAILABLE` | 投屏窗口不存在或 runtime 不支持对应窗口操作。 |
| `CAST_AUDIO_NOT_SUPPORTED` | UxPlay 或系统音频能力不可用。 |
| `INTERNAL_ERROR` | 内部错误。 |

## 8. 待评审问题

- 外部控制口是否允许 LAN 访问？如果允许，是否必须启用 token/HMAC 和 Origin 白名单？
- `getPinCode` 是否允许返回明文 PIN？PIN event 是否也允许包含明文？
- `controlPort` 是 runtime 配置项、Launcher 配置项，还是只读运行状态？
- `quitRuntime` 和 `restartRuntime` 是否只给调试、运维或 admin scope？
- 投屏窗口和 PIN 窗口是否一定独立？如果产品只有一个窗口，需定义 `cast.showPinCode` 与 `cast.showWindow` 的 UI 映射。
- `frameStats` 的频率、字段和性能开销是否默认开启，还是按订阅或调试模式启用？
- 是否需要保留 legacy method alias 用于过渡期兼容，还是外部新接口直接只接受 `cast.*`？
