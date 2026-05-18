# UxPlay / Electron Demo WebSocket 协议说明

本文档描述当前代码里的 WebSocket 协议现状，覆盖两条链路：

- Electron demo 到 UxPlay 的内部控制链路：`ws://127.0.0.1:7001/`
- 外部控制端到 Electron demo 的应用控制链路：`ws://127.0.0.1:7010/`

## 1. 总览

```text
外部控制端
    |
    | WebSocket, JSON, 默认 7010
    v
Electron demo
    |
    | WebSocket, JSON, 默认 7001
    v
UxPlay WebSocket control server
```

Electron 启动 UxPlay 时会传入：

```text
-ws-enable -ws-port <UXPLAY_WS_PORT>
```

并通过子进程环境变量传入同一个会话 token：

```text
UXPLAY_CONTROL_TOKEN=<48 hex chars>
```

Electron 自己对外暴露的控制口默认也使用同一个会话 token，除非设置了 `UXPLAY_APP_WS_TOKEN`。

## 2. 公共消息格式

所有 payload 都是 WebSocket text frame，内容为 JSON。

### 请求

```json
{
  "type": "request",
  "id": "client-generated-id",
  "op": "operationName",
  "data": {}
}
```

### 响应

```json
{
  "type": "response",
  "id": "client-generated-id",
  "ok": true,
  "data": {}
}
```

失败响应：

```json
{
  "type": "response",
  "id": "client-generated-id",
  "ok": false,
  "data": {},
  "error": "ERROR_CODE"
}
```

Electron demo 对外控制口的失败响应会把详细错误放在 `data.error`：

```json
{
  "type": "response",
  "id": "client-generated-id",
  "ok": false,
  "data": {
    "error": {
      "code": "INVALID_PIN",
      "message": "PIN must be exactly 4 numeric digits.",
      "httpStatus": 400
    }
  },
  "error": "INVALID_PIN"
}
```

### 事件

```json
{
  "type": "event",
  "op": "eventName",
  "data": {}
}
```

## 3. 认证

每条 WebSocket 连接都必须先发送 `auth` 请求，认证通过后才能调用其他操作。

```json
{
  "type": "request",
  "id": "auth-1",
  "op": "auth",
  "data": {
    "token": "<token>"
  }
}
```

成功响应：

```json
{
  "type": "response",
  "id": "auth-1",
  "ok": true,
  "data": {
    "message": "authenticated"
  }
}
```

认证失败返回 `UNAUTHORIZED`。Electron demo 的应用控制口会在未授权时关闭连接，close code 为 `1008`。

## 4. UxPlay 内部控制口

### 地址和限制

- 默认地址：`ws://127.0.0.1:7001/`
- UxPlay CLI：`-ws-enable -ws-port <port>`
- Electron 环境变量：`UXPLAY_WS_ENABLE`, `UXPLAY_WS_PORT`
- 只监听 `127.0.0.1`
- WebSocket URI 只接受 `/`
- `Origin` 为空时允许；非空时必须包含 `127.0.0.1` 或 `localhost`
- 当前 Electron 客户端最大接收 payload 为 `16 KiB`

Electron 调 UxPlay 时有两种连接方式：

- 控制请求使用短连接：连接，`auth`，发送一个业务请求，收到响应后关闭
- 事件订阅使用长连接：连接，`auth`，持续接收 `event`

### 操作

#### `setPin`

设置 4 位数字 PIN。

请求：

```json
{
  "type": "request",
  "id": "set-pin-1",
  "op": "setPin",
  "data": {
    "pin": "1234"
  }
}
```

响应：

```json
{
  "type": "response",
  "id": "set-pin-1",
  "ok": true,
  "data": {
    "pin": "1234"
  }
}
```

错误：`INVALID_PARAMS`, `INVALID_STATE`, `UNAUTHORIZED`。

#### `getPin`

查询当前 PIN。未启用 PIN 时返回 `null`。

```json
{
  "type": "request",
  "id": "get-pin-1",
  "op": "getPin",
  "data": {}
}
```

```json
{
  "type": "response",
  "id": "get-pin-1",
  "ok": true,
  "data": {
    "pin": "1234"
  }
}
```

#### `setAudio`

设置投屏音频是否启用。

```json
{
  "type": "request",
  "id": "set-audio-1",
  "op": "setAudio",
  "data": {
    "enabled": false
  }
}
```

```json
{
  "type": "response",
  "id": "set-audio-1",
  "ok": true,
  "data": {
    "mirrorAudio": false
  }
}
```

#### `getAudio`

查询投屏音频状态。

```json
{
  "type": "request",
  "id": "get-audio-1",
  "op": "getAudio",
  "data": {}
}
```

```json
{
  "type": "response",
  "id": "get-audio-1",
  "ok": true,
  "data": {
    "mirrorAudio": true
  }
}
```

#### `stop`

停止当前投屏。

```json
{
  "type": "request",
  "id": "stop-1",
  "op": "stop",
  "data": {}
}
```

```json
{
  "type": "response",
  "id": "stop-1",
  "ok": true,
  "data": {
    "message": "casting stopped"
  }
}
```

#### `getStatus`

查询 UxPlay 控制状态。

```json
{
  "type": "request",
  "id": "status-1",
  "op": "getStatus",
  "data": {}
}
```

```json
{
  "type": "response",
  "id": "status-1",
  "ok": true,
  "data": {
    "mirroring": true,
    "client": "iPhone",
    "model": "iPhone",
    "deviceId": "AA:BB:CC:DD:EE:FF",
    "ip": null,
    "mirrorAudio": true,
    "pin": "1234"
  }
}
```

### UxPlay 事件

UxPlay 事件只会广播给已经通过 `auth` 的 WebSocket 连接。内部事件队列最大 128 条，超过时丢弃最旧事件。

#### `mirrorStarted`

```json
{
  "type": "event",
  "op": "mirrorStarted",
  "data": {
    "device": "iPhone",
    "model": "iPhone",
    "deviceId": "AA:BB:CC:DD:EE:FF",
    "ip": null
  }
}
```

#### `mirrorStopped`

```json
{
  "type": "event",
  "op": "mirrorStopped",
  "data": {
    "reason": "teardown"
  }
}
```

`data` 也可能是空对象。

#### `pinChanged`

```json
{
  "type": "event",
  "op": "pinChanged",
  "data": {
    "pin": "5678"
  }
}
```

#### `pinRequired`

```json
{
  "type": "event",
  "op": "pinRequired",
  "data": {
    "pin": "5678"
  }
}
```

#### `audioChanged`

```json
{
  "type": "event",
  "op": "audioChanged",
  "data": {
    "mirrorAudio": false
  }
}
```

### UxPlay 错误码

| 错误码 | 含义 |
| --- | --- |
| `UNAUTHORIZED` | 未认证或 token 错误 |
| `INVALID_PARAMS` | JSON 格式、参数类型或参数值不合法 |
| `INVALID_STATE` | UxPlay 控制对象尚不可用 |
| `INVALID_OP` | 未知操作 |
| `INTERNAL_ERROR` | 兜底内部错误 |

## 5. Electron demo 应用控制口

### 地址和环境变量

- 默认地址：`ws://127.0.0.1:7010/`
- `UXPLAY_APP_WS_ENABLE=0|false`：关闭 Electron 应用控制口
- `UXPLAY_APP_WS_PORT=<port>`：修改端口，默认 `7010`
- `UXPLAY_APP_WS_HOST=<host>`：设置监听 host
- `UXPLAY_APP_WS_ALLOW_LAN=1|true`：允许按 `UXPLAY_APP_WS_HOST` 监听，否则强制 `127.0.0.1`
- `UXPLAY_APP_WS_TOKEN=<token>`：设置外部控制 token；不设置时使用 Electron 本次会话 token

应用控制口最大接收 payload 为 `64 KiB`。

### 状态对象

`getStatus` 和多个事件会返回 Electron 聚合状态，核心字段如下：

```json
{
  "app": {
    "ready": true,
    "packaged": false,
    "name": "UxPlay SharedTexture"
  },
  "uxplay": {
    "ready": true,
    "pid": 12345,
    "httpPort": 45678,
    "ws": {
      "enabled": true,
      "host": "127.0.0.1",
      "port": 7001,
      "url": "ws://127.0.0.1:7001/"
    }
  },
  "electronWs": {
    "enabled": true,
    "host": "127.0.0.1",
    "port": 7010,
    "url": "ws://127.0.0.1:7010/",
    "allowLan": false,
    "connectedClients": 1,
    "authenticatedClients": 1
  },
  "casting": {
    "active": false,
    "mirrorSessionActive": false,
    "frameStats": {
      "received": 0,
      "queued": 0,
      "sent": 0,
      "releases": 0,
      "importFailures": 0,
      "sendFailures": 0,
      "sendTimeouts": 0,
      "pending": false,
      "sending": false
    }
  },
  "pinState": {
    "value": "1234",
    "updating": false,
    "window": {
      "available": true,
      "visible": false
    }
  },
  "audio": {
    "mirrorAudioEnabled": true,
    "muted": false,
    "updating": false
  },
  "windows": {
    "cast": {
      "available": true,
      "visible": true,
      "fullscreen": true
    },
    "pin": {
      "available": true,
      "visible": false
    }
  },
  "control": {
    "stopUpdating": false
  },
  "error": null
}
```

为兼容当前 renderer，响应里还保留了一组扁平字段：`ready`, `port`, `wsPort`, `wsUrl`, `appWsPort`, `appWsUrl`, `castingActive`, `pin`, `rotating`, `mirrorAudioEnabled`, `muted`, `audioUpdating`, `stopUpdating`, `isFullscreen`, `windowVisible`, `castWindowVisible`, `pinWindowVisible`。

### 操作

#### `getStatus`

返回完整应用状态。

```json
{
  "type": "request",
  "id": "status-1",
  "op": "getStatus",
  "data": {}
}
```

#### `getPin`

```json
{
  "type": "request",
  "id": "pin-1",
  "op": "getPin",
  "data": {}
}
```

响应数据：

```json
{
  "pin": "1234",
  "updating": false
}
```

#### `rotatePin`

生成随机 4 位 PIN，并通过 UxPlay `setPin` 应用。

```json
{
  "type": "request",
  "id": "rotate-1",
  "op": "rotatePin",
  "data": {}
}
```

#### `setPin`

```json
{
  "type": "request",
  "id": "set-pin-1",
  "op": "setPin",
  "data": {
    "pin": "2468"
  }
}
```

#### `setMuted`

以静音语义控制 UxPlay 音频。`muted=true` 会转成 UxPlay `setAudio enabled=false`。

```json
{
  "type": "request",
  "id": "mute-1",
  "op": "setMuted",
  "data": {
    "muted": true
  }
}
```

#### `setAudio`

直接以 UxPlay 语义控制音频。

```json
{
  "type": "request",
  "id": "audio-1",
  "op": "setAudio",
  "data": {
    "enabled": false
  }
}
```

#### `getAudio`

响应数据：

```json
{
  "mirrorAudioEnabled": true,
  "muted": false,
  "updating": false
}
```

#### `stop` / `stopCasting`

停止当前投屏。两个 op 等价。

```json
{
  "type": "request",
  "id": "stop-1",
  "op": "stopCasting",
  "data": {}
}
```

#### 窗口控制

```json
{ "type": "request", "id": "w1", "op": "showCastWindow", "data": {} }
{ "type": "request", "id": "w2", "op": "hideCastWindow", "data": {} }
{ "type": "request", "id": "w3", "op": "setFullscreen", "data": { "fullscreen": true } }
{ "type": "request", "id": "w4", "op": "showPinWindow", "data": {} }
{ "type": "request", "id": "w5", "op": "hidePinWindow", "data": {} }
```

#### 进程控制

```json
{ "type": "request", "id": "p1", "op": "restartUxPlay", "data": {} }
{ "type": "request", "id": "p2", "op": "quitApp", "data": {} }
```

### Electron demo 事件

Electron demo 会向所有已认证外部客户端广播事件。

| 事件 | data |
| --- | --- |
| `app.ready` | 完整状态对象 |
| `status.changed` | 完整状态对象 |
| `uxplay.ready` | 完整状态对象 |
| `uxplay.exited` | `{ "code": number|null, "signal": string|null }` |
| `error` | `{ "code": string, "message": string, "httpStatus": number, "details"?: object }` |
| `control.portChanged` | `{ "port": number }` |
| `casting.started` | `{ "reason": string, "status": object }` |
| `casting.stopped` | `{ "reason": string, "status": object }` |
| `casting.frameStats` | frame stats 对象 |
| `mirrorStarted` | `{ "reason": string }`，兼容事件 |
| `mirrorStopped` | `{ "reason": string }`，兼容事件 |
| `pin.required` | `{ "pin": string|null, "reason": string }` |
| `pinRequired` | `{ "pin": string|null, "reason": "uxplay-event" }`，兼容事件 |
| `pin.accepted` | `{ "pin": string|null, "reason": string }` |
| `pin.hidden` | `{ "pin": string|null }` |
| `pin.changed` | `{ "port": number|null, "pin": string, "source"?: string }` |
| `pinChanged` | `{ "pin": string }`，兼容事件 |
| `audio.changed` | `{ "mirrorAudioEnabled": boolean, "muted": boolean, "source"?: string }` |
| `audioChanged` | `{ "mirrorAudio": boolean }`，兼容事件 |
| `window.changed` | `{ "window": "cast"|"pin", "action": string, "reason"?: string, "state": object }` |

## 6. 推荐调用流程

外部控制 Electron demo：

```text
1. 连接 ws://127.0.0.1:7010/
2. 发送 auth
3. 发送 getStatus，拿到当前 UxPlay 和窗口状态
4. 根据需要调用 rotatePin / setPin / setMuted / stopCasting / 窗口控制
5. 持续监听 status.changed、casting.started、casting.stopped、pin.changed、audio.changed
```

直接控制 UxPlay：

```text
1. 连接 ws://127.0.0.1:7001/
2. 发送 auth，token 必须等于 UxPlay 环境变量 UXPLAY_CONTROL_TOKEN
3. 调用 setPin / getPin / setAudio / getAudio / stop / getStatus
4. 如需事件，保持连接并监听 mirrorStarted、mirrorStopped、pinChanged、pinRequired、audioChanged
```

## 7. 当前实现注意事项

- UxPlay 控制口是本地 loopback 控制口，不面向局域网开放。
- Electron 应用控制口默认也是本地监听；只有设置 `UXPLAY_APP_WS_ALLOW_LAN=1` 后才会使用 `UXPLAY_APP_WS_HOST`。
- Electron 调 UxPlay 的控制请求目前是一次请求一条短连接，不是复用同一个请求连接。
- UxPlay 事件和 Electron 应用事件都没有订阅过滤，认证后的客户端会收到全部广播事件。
- PIN 必须是 4 位数字字符串，例如 `"0007"`。
- `getStatus.data.ip` 当前为 `null`。
