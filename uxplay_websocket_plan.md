# 📄 plan.md — UxPlay WebSocket Control Protocol (Minimal Version)

## 0. Goal

Implement a **minimal WebSocket control layer** for UxPlay to allow Electron apps to:

- Start / stop mirroring
- Set password / PIN
- Query runtime status
- Receive basic events

### Non-goals

- No OBS-style complexity
- No batch requests
- No subscriptions / filters
- No protocol version negotiation

------

## 1. Architecture Overview

### Components

```
Electron App  <--WebSocket-->  UxPlay Control Server (C++)
                                      |
                                      v
                                  UxPlay Core (CLI / embedded)
```

### Strategy

- Wrap UxPlay process (or internal API if you改源码)
- Expose control via WebSocket server
- Stateless request-response model + simple event push

------

## 2. Transport Layer

- Protocol: WebSocket
- Payload: JSON
- Port: 7001 (configurable)

### C++ library options

Pick ONE:

- Simple: Mongoose (recommended)
- Alternative: libwebsockets

------

## 3. Message Protocol

### 3.1 Base Structure

```json
{
  "type": "request | response | event",
  "id": "string (optional for event)",
  "op": "string",
  "data": {}
}
```

------

## 4. API Design

### 4.1 Requests

#### auth

```json
{
  "type": "request",
  "id": "1",
  "op": "auth",
  "data": { "token": "xxx" }
}
```

------

#### startMirror

Starts UxPlay process

```json
{
  "op": "startMirror",
  "data": {
    "name": "UxPlay",
    "resolution": "1920x1080"
  }
}
```

------

#### stopMirror

```json
{
  "op": "stopMirror"
}
```

------

#### setPassword

Maps to UxPlay `-pw` option ([ManKier](https://www.mankier.com/1/uxplay?utm_source=chatgpt.com))

```json
{
  "op": "setPassword",
  "data": { "password": "123456" }
}
```

------

#### getStatus

```json
{
  "op": "getStatus"
}
```

Response:

```json
{
  "mirroring": true,
  "client": "iPhone",
  "ip": "192.168.1.10"
}
```

------

## 5. Responses

### Success

```json
{
  "type": "response",
  "id": "1",
  "ok": true,
  "data": {}
}
```

### Error

```json
{
  "type": "response",
  "id": "1",
  "ok": false,
  "error": "INVALID_STATE"
}
```

------

## 6. Events

### mirrorStarted

```json
{
  "type": "event",
  "op": "mirrorStarted",
  "data": {
    "device": "iPhone"
  }
}
```

------

### mirrorStopped

```json
{
  "type": "event",
  "op": "mirrorStopped"
}
```

------

### clientConnected

### clientDisconnected

------

## 7. Authentication

### Rule

- First message MUST be `auth`
- Before auth:
  - Only allow `auth`
- After auth:
  - Allow all requests

### Storage

- Token stored in config file
- No encryption (local use only)

------

## 8. UxPlay Integration Strategy

### Option A (recommended)

Spawn process:

```bash
uxplay -pw 123456 -n MyServer
```

Control via:

- start → spawn process
- stop → kill process

------

### Option B (advanced)

Modify UxPlay source:

- expose start/stop as functions
- embed control server inside

------

## 9. Electron SDK (Client Wrapper)

### API

```ts
await client.connect()

await client.auth(token)

await client.startMirror()
await client.stopMirror()

const status = await client.getStatus()

client.on("mirrorStarted", cb)
```

------

### Implementation Notes

- Maintain request map (id → resolve)
- Auto reconnect
- Timeout handling

------

## 10. Error Codes

Define minimal set:

```
UNAUTHORIZED
INVALID_OP
INVALID_STATE
INTERNAL_ERROR
```

------

## 11. Future Extensions (Optional)

DO NOT implement now, but keep compatible:

- heartbeat (ping/pong)
- multi-client support
- config persistence
- device whitelist (maps to UxPlay restrict feature) ([GitHub](https://github.com/FDH2/UxPlay?utm_source=chatgpt.com))

------

## 12. Milestones

### Phase 1 (MVP)

- WebSocket server
- auth
- start/stop
- getStatus

------

### Phase 2

- events
- password control
- Electron SDK

------

### Phase 3

- stability (reconnect / error handling)
- packaging

------

## 13. Success Criteria

- Electron can control UxPlay in <10 lines code
- No protocol doc needed for usage
- Total API count ≤ 6

------

# ✅ One-line Philosophy

> Build a **control channel**, not a “remote OBS”.

