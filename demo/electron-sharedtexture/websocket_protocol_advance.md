# 协议介绍

## 基本消息模型

下面是一个基本的消息模型的构成

```Shell
{
  "sid": string, 
  "op": number,
  "d": object
}
```

- `sid` - `SessionId`，方便用于追踪一次完整的连接的生命周期
- `op` - `OpCodes`，操作码，标识每次操作的类型，后面会做详细的讲解
- `d` - `object`，操作相关的参数、回复等内容都在内

### 操作数op（OpCodes，op）

| **字段值** | **值解析**                     |
| ---------- | ------------------------------ |
| 0          | Hello 用于握手，创建会话       |
| 1          | HelloAck 握手响应报文类型      |
| 5          | Subscribe 订阅请求             |
| 6          | Event 事件通知请求             |
| 7          | Request 资源方法请求           |
| 8          | RequestResponse 请求的响应报文 |
| 14         | bye 结束会话请求报文           |
| 15         | byeAck bye请求的响应报文       |

## 内容对象模型

### 内容字段列表

| **字段** | **字段介绍**                                                 |
| -------- | ------------------------------------------------------------ |
| method   | 请求方法名称                                                 |
| params   | 请求参数                                                     |
| id       | 请求id,每一个method请求，都需要携带该字段，用来唯一标识一个请求，响应的报文也要携带该字段 |
| status   | 请求响应的状态信息                                           |
| code     | 错误码                                                       |
| comment  | 错误描述信息                                                 |
| result   | 请求结果信息                                                 |
| event    | 事件名称                                                     |
| data     | 事件内容信息                                                 |

### 请求类型（op=7）

关键字段：

```Shell
{
  "id": string,
  "method": string,
  "params": object(optional),
}
```

消息样例：

```Shell
{
  "d": {
      "id":"u23hduJE3",
      "method":"SetDeviceName",
      "params":{
          "name":"abc"
      }
  }
  "op": 7,
  "sid": "28378462323"
}
```

### 回复类型（op=8）

关键字段

```Shell
{
    "status": {
          "result": bool,
          "code": number,
          "comment": string(optional)
     },
     "result":object(optional)
}
```

成功回复：

```Shell
{
  "d": {
      "id":"u23hduJE3",
      "method":"SetDeviceName",
      "status": {
           "code": 100,
           "result": true
      },
      "result":{
          "name":"abc"
      }
  }
  "op": 8,
  "sid": "28378462323"
}
```

失败回复：

```Shell
{
  "d": {
      "id":"u23hduJE3",
      "method":"SetDeviceName",
      "status": {
           "code": 300,
           "result": false,
           "comment": "Name couldn't be set"
      }
  }
  "op": 8,
  "sid": "28378462323"
}
```

### 事件类型

关键字段：

```Shell
{
  "event": string,
  "data": object(optional)
}
```

消息样例：

```Shell
{
    "op":6,
    "sid":"28378462323",
    "d":{
        "event":"DeviceNameChanged",
        "data":{
            "name":"USB-2",
            "type":"USB-2",
            "ability":"video/audio",
            "streamContent":"",
            "inputSourceId"：2,
            "status":false，
        }
    }
}
```

## 基本交互流程

1. 连接握手-心跳-断联
2. 心跳包
3. 设备注册
4. 断开连接（挥手协议）

## 具体消息

1. 连接握手
2. 心跳包
3. 设备注册
4. 断开连接
5. 设备控制

# 参考：

https://github.com/obsproject/obs-websocket

https://github.com/obs-websocket-community-projects/obs-websocket-js