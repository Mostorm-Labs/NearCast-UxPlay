````markdown
# uxplay Windows 投屏窗口半透明按钮实现方案

## 推荐方案

**Win32 主窗口 + GStreamer 视频子窗口 + WebView2 半透明按钮层**

```text
Win32 主窗口
├── Video HWND
│   └── GStreamer d3d11videosink 投屏画面
└── WebView2 Overlay HWND
    └── HTML/CSS 半透明按钮
````

## 为什么选这个方案

* Windows 单平台最稳定
* 视频继续走 GStreamer 原生硬件渲染
* 按钮样式可复用 Electron 的 HTML/CSS
* 半透明、圆角、hover、动画都容易实现
* 不需要引入 Qt / GTK
* 后续按钮逻辑可通过 WebView2 `postMessage` 回到 C/C++

## 技术选型

| 模块    | 方案                         |
| ----- | -------------------------- |
| 主窗口   | Win32                      |
| 视频渲染  | GStreamer `d3d11videosink` |
| 视频嵌入  | `GstVideoOverlay`          |
| 按钮 UI | WebView2                   |
| 样式同步  | 复用 Electron CSS            |
| 按钮通信  | WebView2 `postMessage`     |

## 窗口结构

```text
Main HWND
├── video_hwnd
│   └── d3d11videosink render target
└── overlay_hwnd
    └── WebView2 transparent background
        ├── Button 1
        └── Button 2
```

## GStreamer 侧关键代码

```c
gst_video_overlay_set_window_handle(
    GST_VIDEO_OVERLAY(video_sink),
    (guintptr)video_hwnd
);
```

建议使用：

```text
d3d11videosink
```

作为 Windows 下的视频 sink。

## WebView2 按钮层示例

```html
<div class="controls">
  <button id="btn1">按钮 1</button>
  <button id="btn2">按钮 2</button>
</div>
```

```css
body {
  margin: 0;
  background: transparent;
  overflow: hidden;
}

.controls {
  position: absolute;
  right: 24px;
  bottom: 24px;
  display: flex;
  gap: 12px;
}

button {
  background: rgba(30, 30, 30, 0.45);
  color: white;
  border: 1px solid rgba(255, 255, 255, 0.18);
  border-radius: 18px;
  padding: 8px 14px;
  backdrop-filter: blur(12px);
  cursor: pointer;
}

button:hover {
  background: rgba(30, 30, 30, 0.65);
}
```

## 按钮事件通信

WebView2 页面中：

```js
document.getElementById("btn1").onclick = () => {
  chrome.webview.postMessage({ type: "button1" });
};

document.getElementById("btn2").onclick = () => {
  chrome.webview.postMessage({ type: "button2" });
};
```

C++ 侧接收消息后控制 uxplay 行为，例如：

```text
button1 → 断开投屏
button2 → 全屏 / 退出全屏
```

## 实现步骤

1. 创建 Win32 主窗口
2. 创建 `video_hwnd` 子窗口
3. 创建 WebView2 overlay 子窗口
4. 将 GStreamer `d3d11videosink` 绑定到 `video_hwnd`
5. WebView2 加载本地 HTML/CSS
6. 设置 WebView2 背景透明
7. 同步主窗口 resize，让视频层和 overlay 层一起调整
8. 通过 `postMessage` 接收按钮事件
9. 将按钮事件映射到 uxplay 控制逻辑

## 需要注意的问题

### 1. Overlay 层透明

WebView2 需要开启透明背景，否则会遮挡视频画面。

### 2. 窗口层级

`overlay_hwnd` 必须在 `video_hwnd` 上方。

### 3. Resize 同步

主窗口尺寸变化时，同时调整：

```text
video_hwnd
overlay_hwnd
WebView2 bounds
```

### 4. 全屏模式

全屏时保持：

```text
video_hwnd = 全屏区域
overlay_hwnd = 全屏区域
按钮位置 = 右下角 / 其他指定位置
```

### 5. 鼠标事件

overlay 层只处理按钮区域点击，非按钮区域最好不要阻塞视频窗口事件。

## 最终结论

Windows 单平台最推荐：

```text
Win32 + GStreamer d3d11videosink + GstVideoOverlay + WebView2 Overlay
```

这个方案兼顾：

* 视频性能
* UI 样式同步
* 半透明按钮效果
* Electron CSS 复用
* 后续维护成本

```
```
