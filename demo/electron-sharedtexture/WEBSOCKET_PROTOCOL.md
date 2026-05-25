# UxPlay / Electron Demo WebSocket 协议说明

本文档只描述 `websocket_protocol_advance.md` 中的增强消息模型。当前实现不再兼容轻量协议
`type/id/op/data/ok/error`；所有 WebSocket text frame 都必须是 JSON，并使用 `sid/op/d` 外层结构。

## 1. 链路

```text
外部控制端
    | ws://127.0.0.1:7010/
    v
Electron demo 应用控制口
    | ws://127.0.0.1:7001/
    v
UxPlay WebSocket control server
```

Electron 启动 UxPlay 时会传入：

```text
-ws-enable -ws-port <UXPLAY_WS_PORT>
```

并通过环境变量传入同一个控制 token：

```text
UXPLAY_CONTROL_TOKEN=<48 hex chars>
```

Electron 对外控制口默认也使用这个 token，除非设置 `UXPLAY_APP_WS_TOKEN`。

## 2. 增强消息模型

### Envelope

```json
{
  "sid": "session-id",
  "op": 7,
  "d": {}
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `sid` | string | SessionId，用于追踪一次 WebSocket 连接生命周期。客户端可在 `Hello` 里提供；为空时服务端生成。 |
| `op` | number | OpCode，标识消息类型。 |
| `d` | object | 当前消息内容。请求参数、响应结果和事件数据都放在这里。 |

### OpCode

| op | 名称 | 说明 |
| --- | --- | --- |
| `0` | `Hello` | 客户端握手，创建或声明会话。 |
| `1` | `HelloAck` | 服务端握手响应，返回 `sid`、协议版本和能力。 |
| `6` | `Event` | 服务端事件通知。 |
| `7` | `Request` | 资源方法请求。 |
| `8` | `RequestResponse` | 请求响应。 |
| `14` | `Bye` | 客户端结束会话。 |
| `15` | `ByeAck` | 服务端结束会话响应。 |

### Request `op=7`

```json
{
  "sid": "session-id",
  "op": 7,
  "d": {
    "id": "set-pin-1",
    "method": "setPin",
    "params": {
      "pin": "1234"
    }
  }
}
```

### Response `op=8`

成功：

```json
{
  "sid": "session-id",
  "op": 8,
  "d": {
    "id": "set-pin-1",
    "method": "setPin",
    "status": {
      "result": true,
      "code": 100
    },
    "result": {
      "pin": "1234"
    }
  }
}
```

失败：

```json
{
  "sid": "session-id",
  "op": 8,
  "d": {
    "id": "set-pin-1",
    "method": "setPin",
    "status": {
      "result": false,
      "code": 300,
      "comment": "INVALID_PARAMS"
    }
  }
}
```

### Event `op=6`

```json
{
  "sid": "session-id",
  "op": 6,
  "d": {
    "event": "pinChanged",
    "data": {
      "pin": "5678"
    }
  }
}
```

## 3. 交互流程

```text
1. 建立 WebSocket 连接
2. 客户端发送 Hello(op=0)
3. 服务端返回 HelloAck(op=1)，包含 sid、protocolVersion、capabilities、maxPayload
4. 客户端发送 auth Request(op=7, method=auth)
5. 服务端返回 RequestResponse(op=8)
6. 客户端发送业务 Request(op=7)，服务端用 RequestResponse(op=8) 回复
7. 服务端可随时推送 Event(op=6)
8. 客户端主动结束时发送 Bye(op=14)，服务端返回 ByeAck(op=15)
```

认证请求示例：

```json
{
  "sid": "session-id",
  "op": 7,
  "d": {
    "id": "auth-1",
    "method": "auth",
    "params": {
      "token": "<token>"
    }
  }
}
```

认证失败会返回 `UNAUTHORIZED`，Electron 应用控制口会关闭连接，close code 为 `1008`。

## 4. UxPlay 内部控制口

- 地址：`ws://127.0.0.1:7001/`
- CLI：`-ws-enable -ws-port <port>`
- 只监听 `127.0.0.1`
- WebSocket URI 只接受 `/`
- `Origin` 为空时允许；非空时必须包含 `127.0.0.1` 或 `localhost`
- 最大接收 payload：`16 KiB`

### 操作

| method | params | result |
| --- | --- | --- |
| `auth` | `{ "token": string }` | `{ "message": "authenticated" }` |
| `setPin` | `{ "pin": "1234" }` | `{ "pin": "1234" }` |
| `getPin` | `{}` | `{ "pin": "1234" }` 或 `{ "pin": null }` |
| `setAudio` | `{ "enabled": boolean }` | `{ "mirrorAudio": boolean }` |
| `getAudio` | `{}` | `{ "mirrorAudio": boolean }` |
| `stop` | `{}` | `{ "message": "casting stopped" }` |
| `getStatus` | `{}` | UxPlay 状态对象 |

### 事件

UxPlay 事件只会广播给已经通过 `auth` 的连接。

| event | data |
| --- | --- |
| `mirrorStarted` | `{ "device": string, "model": string, "deviceId": string, "ip": null }` |
| `mirrorStopped` | `{ "reason": string }` 或 `{}` |
| `pinChanged` | `{ "pin": string }` |
| `pinRequired` | `{ "pin": string }` |
| `audioChanged` | `{ "mirrorAudio": boolean }` |

## 5. Electron 应用控制口

- 地址：`ws://127.0.0.1:7010/`
- `UXPLAY_APP_WS_ENABLE=0|false`：关闭 Electron 应用控制口
- `UXPLAY_APP_WS_PORT=<port>`：修改端口，默认 `7010`
- `UXPLAY_APP_WS_HOST=<host>`：设置监听 host
- `UXPLAY_APP_WS_ALLOW_LAN=1|true`：允许按 `UXPLAY_APP_WS_HOST` 监听，否则强制 `127.0.0.1`
- `UXPLAY_APP_WS_TOKEN=<token>`：设置外部控制 token；不设置时使用 Electron 本次会话 token
- 最大接收 payload：`64 KiB`

### 操作

| method | params | result |
| --- | --- | --- |
| `auth` | `{ "token": string }` | `{ "message": "authenticated" }` |
| `getStatus` | `{}` | 完整应用状态对象 |
| `getServerName` | `{}` | `{ "serverName": string }` |
| `setServerName` / `setUxPlayServerName` | `{ "serverName": string }` 或 `{ "name": string }` | server name 更新结果 |
| `getPin` | `{}` | `{ "pin": string|null, "updating": boolean }` |
| `rotatePin` | `{}` | 新 PIN 和应用结果 |
| `setPin` | `{ "pin": "2468" }` | PIN 应用结果 |
| `setMuted` | `{ "muted": boolean }` | 音频应用结果 |
| `setAudio` | `{ "enabled": boolean }` | 音频应用结果 |
| `getAudio` | `{}` | `{ "mirrorAudioEnabled": boolean|null, "muted": boolean|null, "updating": boolean }` |
| `stop` / `stopCasting` | `{}` | 停止投屏结果 |
| `showCastWindow` / `hideCastWindow` | `{}` | 完整应用状态对象 |
| `setFullscreen` | `{ "fullscreen": boolean }` | 窗口状态和完整应用状态 |
| `setAlwaysOnTop` | `{ "alwaysOnTop": boolean }` | 窗口状态和完整应用状态 |
| `showPinWindow` / `hidePinWindow` | `{}` | 完整应用状态对象 |
| `restartUxPlay` | `{}` | 完整应用状态对象 |
| `quitApp` | `{}` | `{ "message": "quitting" }` |

### 事件

Electron 应用控制口会向所有已认证客户端广播事件。

| event | data |
| --- | --- |
| `app.ready` | 完整应用状态对象 |
| `status.changed` | 完整应用状态对象 |
| `uxplay.ready` | 完整应用状态对象 |
| `uxplay.exited` | `{ "code": number|null, "signal": string|null }` |
| `error` | `{ "code": string, "message": string, "httpStatus": number, "details"?: object }` |
| `serverName.changed` | `{ "serverName": string, "previousServerName": string, "reason": string }` |
| `control.portChanged` | `{ "port": number }` |
| `casting.started` / `casting.stopped` | `{ "reason": string, "status": object }` |
| `casting.frameStats` | frame stats 对象 |
| `pin.required` | `{ "pin": string|null, "reason": string }` |
| `pin.accepted` | `{ "pin": string|null, "reason": string }` |
| `pin.hidden` | `{ "pin": string|null }` |
| `pin.changed` | `{ "port": number|null, "pin": string, "source"?: string }` |
| `audio.changed` | `{ "mirrorAudioEnabled": boolean, "muted": boolean, "source"?: string }` |
| `window.changed` | `{ "window": "cast"|"pin", "action": string, "reason"?: string, "state": object }` |

## 6. 错误码

| code | 含义 |
| --- | --- |
| `UNAUTHORIZED` | 未认证或 token 错误 |
| `INVALID_PARAMS` | JSON 格式、参数类型或参数值不合法 |
| `INVALID_STATE` | 控制对象暂不可用 |
| `INVALID_OP` | 未知操作或不支持的 opcode |
| `INTERNAL_ERROR` | 内部错误 |

旧轻量协议请求会被视为无效请求，不再转换或兼容。
