# 📄 plan.md — UxPlay WebSocket Control Protocol (Optimized)

## 0. Goal

Implement a **WebSocket control layer** for UxPlay to allow Electron apps to:

- Start / stop mirroring
- Set password / PIN
- Query runtime status
- Receive real-time events (casting start/stop)

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
                                UxPlay Core (CLI)
```

### Strategy

- Wrap UxPlay process (spawn/monitor/kill)
- Expose control via WebSocket server **parallel to existing HTTP API**
- Stateless request-response model + simple event push
- **Reuse existing HTTP API path structure** for consistency

### Backward Compatibility

WebSocket control runs **side-by-side** with existing HTTP API:
- HTTP port: Auto-assigned (detected from UxPlay log output)
- WebSocket port: Configurable (default: `7001`)
- Both APIs share the same underlying control state

------

## 2. Transport Layer

- Protocol: WebSocket (RFC 6455)
- Payload: JSON
- Port: `7001` (configurable via `-ws-port` flag)
- Origin restriction: `127.0.0.1` only (local control only)

### C++ library options

Pick ONE:
- Simple: Mongoose (recommended, lightweight)
- Alternative: libwebsockets

------

## 3. Message Protocol

### 3.1 Base Structure

**Request:**
```json
{
  "type": "request",
  "id": "string (unique per request)",
  "op": "string",
  "data": {}
}
```

**Response:**
```json
{
  "type": "response",
  "id": "string (matches request)",
  "ok": true | false,
  "data": {},
  "error": "string (optional, when ok=false)"
}
```

**Event (Server Push):**
```json
{
  "type": "event",
  "op": "string",
  "data": {}
}
```

### 3.2 Authentication Token

Mirror session token (random 48-char hex string):
- Generated at UxPlay startup
- Passed via `UXPLAY_CONTROL_TOKEN` environment variable to Electron
- **Same token** used for both HTTP and WebSocket authentication

------

## 4. API Design

### 4.1 Requests (WebSocket)

#### auth

**Required as first message** - validates control token before other operations

```json
{
  "type": "request",
  "id": "1",
  "op": "auth",
  "data": { "token": "xxx" }
}
```

Response:
```json
{
  "type": "response",
  "id": "1",
  "ok": true,
  "data": { "message": "authenticated" }
}
```

#### setPin

Maps to existing `PUT /api/pin`

```json
{
  "type": "request",
  "id": "2",
  "op": "setPin",
  "data": { "pin": "1234" }
}
```

#### getPin

Get current PIN status

```json
{
  "type": "request",
  "id": "3",
  "op": "getPin"
}
```

Response:
```json
{
  "type": "response",
  "id": "3",
  "ok": true,
  "data": { "pin": "1234" }
}
```

#### setAudio

Maps to existing `PUT /api/audio`

```json
{
  "type": "request",
  "id": "4",
  "op": "setAudio",
  "data": { "enabled": true }
}
```

#### getAudio

Maps to existing `GET /api/audio`

```json
{
  "type": "request",
  "id": "5",
  "op": "getAudio"
}
```

Response:
```json
{
  "type": "response",
  "id": "5",
  "ok": true,
  "data": { "mirrorAudio": true }
}
```

#### stop

Maps to existing `POST /api/stop`

```json
{
  "type": "request",
  "id": "6",
  "op": "stop"
}
```

#### getStatus

Combined status query

```json
{
  "type": "request",
  "id": "7",
  "op": "getStatus"
}
```

Response:
```json
{
  "type": "response",
  "id": "7",
  "ok": true,
  "data": {
    "mirroring": true,
    "client": "iPhone",
    "ip": "192.168.1.10",
    "mirrorAudio": true,
    "pin": "1234"
  }
}
```

------

## 5. Events (Server Push)

### mirrorStarted

```json
{
  "type": "event",
  "op": "mirrorStarted",
  "data": {
    "device": "iPhone",
    "ip": "192.168.1.10"
  }
}
```

### mirrorStopped

```json
{
  "type": "event",
  "op": "mirrorStopped"
}
```

### pinChanged

```json
{
  "type": "event",
  "op": "pinChanged",
  "data": { "pin": "5678" }
}
```

### audioChanged

```json
{
  "type": "event",
  "op": "audioChanged",
  "data": { "mirrorAudio": false }
}
```

------

## 6. Authentication

### Rule

- First message **MUST** be `auth`
- Before auth: Only `auth` is allowed
- After auth: All requests allowed

### Token Generation

```bash
# UxPlay generates token on startup
UXPLAY_CONTROL_TOKEN=$(openssl rand -hex 24)
export UXPLAY_CONTROL_TOKEN
```

### Storage

- Token stored in memory only (ephemeral per session)
- Passed to Electron via environment variable
- No persistent storage required

------

## 7. UxPlay Integration Strategy

### Current Implementation (Reference)

Electron spawns UxPlay with these arguments:
```bash
uxplay -n "UxPlay SharedTexture" -stpid <pid> -pw -nh -fs -nohold
```

Control flow:
1. Electron parses UxPlay stderr to extract HTTP port
2. HTTP requests sent to `http://127.0.0.1:<port>/api/*`
3. WebSocket connects to `ws://127.0.0.1:7001/`

### WebSocket Integration

Add new command-line flags:
- `-ws-port <port>`: Set WebSocket control port (default: 7001)
- `-ws-enable`: Enable WebSocket control server

------

## 8. Electron SDK (Client Wrapper)

### API

```typescript
class UxPlayClient {
  async connect(url: string): Promise<void>
  async auth(token: string): Promise<void>
  
  async setPin(pin: string): Promise<void>
  async getPin(): Promise<string>
  async rotatePin(): Promise<string>
  
  async setAudio(enabled: boolean): Promise<void>
  async getAudio(): Promise<boolean>
  
  async stop(): Promise<void>
  async getStatus(): Promise<Status>
  
  on(event: 'mirrorStarted', handler: (data) => void): void
  on(event: 'mirrorStopped', handler: () => void): void
  on(event: 'pinChanged', handler: (pin: string) => void): void
  on(event: 'audioChanged', handler: (enabled: boolean) => void): void
  
  disconnect(): void
}
```

### Implementation Notes

- Maintain request map (`id` → `resolve`)
- Auto-reconnect with exponential backoff
- Timeout handling (3000ms default)
- Event listener management

------

## 9. Error Codes

```
UNAUTHORIZED      # Invalid or missing token
INVALID_OP        # Unknown operation
INVALID_STATE     # Operation not allowed in current state
INVALID_PARAMS    # Invalid request parameters
INTERNAL_ERROR    # Server-side error
TIMEOUT           # Request timeout
```

------

## 10. Feature Comparison

| Feature | HTTP API | WebSocket |
|---------|----------|-----------|
| PIN control | ✅ | ✅ |
| Audio control | ✅ | ✅ |
| Stop casting | ✅ | ✅ |
| Status query | ✅ | ✅ |
| Real-time events | ❌ | ✅ |
| Auto-reconnect | ❌ | ✅ |
| Lower latency | ❌ | ✅ |

------

## 11. Milestones

### Phase 1 (MVP)

- WebSocket server implementation
- Basic authentication
- `setPin`, `getPin`, `stop` operations
- `mirrorStarted`, `mirrorStopped` events

### Phase 2

- Audio control (`setAudio`, `getAudio`)
- Full status query
- `pinChanged`, `audioChanged` events
- Electron SDK wrapper

### Phase 3

- Stability improvements
- Auto-reconnect with backoff
- Connection health monitoring

------

## 12. Success Criteria

- Electron can control UxPlay in <10 lines of code
- Real-time event delivery (<100ms latency)
- API compatible with existing HTTP interface
- Auto-reconnect works reliably

------

# ✅ One-line Philosophy

> Build a **real-time control channel** that complements, not replaces, the existing HTTP API.