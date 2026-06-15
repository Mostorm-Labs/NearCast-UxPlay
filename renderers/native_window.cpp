/*
 * Windows native video window support for UxPlay.
 */

#include "native_window.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <WebView2.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

const wchar_t kMainWindowClass[] = L"UxPlayNativeWindow";
const wchar_t kOverlayWindowClass[] = L"UxPlayNativeOverlay";
const wchar_t kPinWindowClass[] = L"UxPlayNativePin";
const int kInitialWindowWidth = 960;
const int kInitialWindowHeight = 540;
const int kToolButtonSize = 36;
const int kToolButtonGap = 8;
const int kToolButtonCount = 2;
const int kToolbarPaddingX = 10;
const int kToolbarPaddingY = 6;
const int kToolbarMarginRight = 12;
const int kPinWindowWidth = 260;
const int kPinWindowHeight = 70;
const int kPinWindowTop = 24;
const UINT_PTR kPinAutoHideTimer = 1;
const UINT kPinAutoHideMs = 6000;
const UINT kNativeWindowDestroyMessage = WM_APP + 64;
const UINT kNativeWindowPinMessage = WM_APP + 65;
const UINT kDefaultDpi = 96;

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

struct NativeWindowState {
    logger_t *logger = nullptr;
    HINSTANCE instance = nullptr;
    HWND main_hwnd = nullptr;
    HWND video_hwnd = nullptr;
    HWND overlay_hwnd = nullptr;
    HWND pin_hwnd = nullptr;
    HMODULE webview_loader = nullptr;
    ICoreWebView2Environment *webview_environment = nullptr;
    ICoreWebView2Controller *webview_controller = nullptr;
    ICoreWebView2 *webview = nullptr;
    ICoreWebView2Controller *pin_webview_controller = nullptr;
    ICoreWebView2 *pin_webview = nullptr;
    EventRegistrationToken web_message_token = {};
    std::thread window_thread;
    std::mutex mutex;
    std::condition_variable ready_cv;
    bool ready = false;
    bool create_failed = false;
    bool fullscreen = false;
    bool running = false;
    bool com_initialized = false;
    bool webview_ready = false;
    bool pin_webview_ready = false;
    bool web_message_token_valid = false;
    bool pin_visible = false;
    UINT dpi = kDefaultDpi;
    RECT restore_rect = {};
    LONG_PTR restore_style = 0;
    LONG_PTR restore_ex_style = 0;
    native_window_action_callback_t action_callback = nullptr;
    void *action_userdata = nullptr;
    std::wstring title;
    std::wstring pin_value = L"----";
};

struct PinUpdate {
    bool show = false;
    wchar_t pin[5] = {};
};

NativeWindowState g_state;

class EnvironmentCreatedHandler;
class ControllerCreatedHandler;
class PinControllerCreatedHandler;
class WebMessageReceivedHandler;

void Log(int level, const char *message) {
    if (g_state.logger) {
        logger_log(g_state.logger, level, "%s", message);
    }
}

void LogHresult(int level, const char *message, HRESULT hr) {
    char buffer[256] = {};
    snprintf(buffer, sizeof(buffer), "%s (hr=0x%08lx)", message, static_cast<unsigned long>(hr));
    Log(level, buffer);
}

void EnableProcessDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
    auto set_awareness_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_awareness_context) {
        if (set_awareness_context(reinterpret_cast<HANDLE>(-4))) {
            return;
        }
        if (set_awareness_context(reinterpret_cast<HANDLE>(-3))) {
            return;
        }
    }

    typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
    auto set_process_dpi_aware = reinterpret_cast<SetProcessDPIAwareFn>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    if (set_process_dpi_aware) {
        set_process_dpi_aware();
    }
}

UINT GetSystemDpiValue() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForSystemFn)(void);
        auto get_dpi_for_system = reinterpret_cast<GetDpiForSystemFn>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi_for_system) {
            UINT dpi = get_dpi_for_system();
            if (dpi > 0) {
                return dpi;
            }
        }
    }

    HDC hdc = GetDC(nullptr);
    if (!hdc) {
        return kDefaultDpi;
    }
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi > 0 ? static_cast<UINT>(dpi) : kDefaultDpi;
}

UINT GetWindowDpiValue(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi_for_window && hwnd) {
            UINT dpi = get_dpi_for_window(hwnd);
            if (dpi > 0) {
                return dpi;
            }
        }
    }
    return g_state.dpi > 0 ? g_state.dpi : GetSystemDpiValue();
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi > 0 ? dpi : kDefaultDpi), static_cast<int>(kDefaultDpi));
}

RECT AdjustWindowRectForDpi(RECT rect, DWORD style, DWORD ex_style, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto adjust_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_for_dpi && adjust_for_dpi(&rect, style, FALSE, ex_style, dpi)) {
            return rect;
        }
    }
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    return rect;
}

template <typename T>
void ReleaseCom(T **value) {
    if (value && *value) {
        (*value)->Release();
        *value = nullptr;
    }
}

std::wstring ToWide(const char *text) {
    if (!text || !*text) {
        return L"UxPlay";
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 0) {
        return L"UxPlay";
    }
    std::wstring wide(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], len);
    return wide;
}

std::wstring BuildWebViewUserDataFolder() {
    wchar_t temp_path[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, temp_path);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }
    std::wstring folder(temp_path);
    if (!folder.empty() && folder.back() != L'\\') {
        folder.push_back(L'\\');
    }
    folder += L"UxPlayWebView2";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

RECT GetWebViewBounds(HWND hwnd) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    UINT dpi = GetWindowDpiValue(hwnd);
    int button_size = ScaleForDpi(kToolButtonSize, dpi);
    int button_gap = ScaleForDpi(kToolButtonGap, dpi);
    int padding_x = ScaleForDpi(kToolbarPaddingX, dpi);
    int padding_y = ScaleForDpi(kToolbarPaddingY, dpi);
    int margin_right = ScaleForDpi(kToolbarMarginRight, dpi);
    int width = button_size + padding_x * 2;
    int height = button_size * kToolButtonCount +
                 button_gap * (kToolButtonCount - 1) +
                 padding_y * 2;
    int client_width = static_cast<int>(client.right - client.left);
    int client_height = static_cast<int>(client.bottom - client.top);
    int left = std::max(0, client_width - margin_right - width);
    int top = std::max(0, (client_height - height) / 2);
    return RECT{left, top, left + width, top + height};
}

RECT GetPinWebViewBounds(HWND hwnd) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    return client;
}

RECT GetPinWindowBounds() {
    RECT work = {};
    HMONITOR monitor = MonitorFromWindow(g_state.main_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work = monitor_info.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = RECT{0, 0, kInitialWindowWidth, kInitialWindowHeight};
    }

    UINT dpi = GetWindowDpiValue(g_state.main_hwnd);
    int pin_width = ScaleForDpi(kPinWindowWidth, dpi);
    int pin_height = ScaleForDpi(kPinWindowHeight, dpi);
    int pin_top = ScaleForDpi(kPinWindowTop, dpi);
    int work_width = static_cast<int>(work.right - work.left);
    int left = static_cast<int>(work.left) + std::max(0, (work_width - pin_width) / 2);
    int top = static_cast<int>(work.top) + pin_top;
    return RECT{left, top, left + pin_width, top + pin_height};
}

RECT GetButtonRect(HWND hwnd, int index) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    UINT dpi = GetWindowDpiValue(hwnd);
    int button_size = ScaleForDpi(kToolButtonSize, dpi);
    int button_gap = ScaleForDpi(kToolButtonGap, dpi);
    int padding_x = ScaleForDpi(kToolbarPaddingX, dpi);
    int padding_y = ScaleForDpi(kToolbarPaddingY, dpi);
    int margin_right = ScaleForDpi(kToolbarMarginRight, dpi);
    int client_width = static_cast<int>(client.right - client.left);
    int client_height = static_cast<int>(client.bottom - client.top);
    int width = button_size + padding_x * 2;
    int height = button_size * kToolButtonCount +
                 button_gap * (kToolButtonCount - 1) +
                 padding_y * 2;
    int left = std::max(0, client_width - margin_right - width + padding_x);
    int top = std::max(0, (client_height - height) / 2 + padding_y);
    top += index * (button_size + button_gap);
    return RECT{left, top, left + button_size, top + button_size};
}

bool PointInRect(const RECT &rect, POINT pt) {
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

void ResizeWebView() {
    if (g_state.webview_controller && g_state.overlay_hwnd) {
        RECT bounds = GetWebViewBounds(g_state.overlay_hwnd);
        g_state.webview_controller->put_Bounds(bounds);
        g_state.webview_controller->NotifyParentWindowPositionChanged();
    }
    if (g_state.pin_webview_controller && g_state.pin_hwnd) {
        RECT bounds = GetPinWebViewBounds(g_state.pin_hwnd);
        g_state.pin_webview_controller->put_Bounds(bounds);
        g_state.pin_webview_controller->NotifyParentWindowPositionChanged();
    }
}

void ResizeChildren(HWND hwnd) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (g_state.video_hwnd) {
        MoveWindow(g_state.video_hwnd, 0, 0, width, height, TRUE);
    }
    if (g_state.overlay_hwnd) {
        MoveWindow(g_state.overlay_hwnd, 0, 0, width, height, TRUE);
        SetWindowPos(g_state.overlay_hwnd, HWND_TOP, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ResizeWebView();
        InvalidateRect(g_state.overlay_hwnd, nullptr, TRUE);
    }
}

void PostFullscreenStateToWebView() {
    if (!g_state.webview) {
        return;
    }
    g_state.webview->PostWebMessageAsJson(g_state.fullscreen ?
                                          L"{\"type\":\"fullscreen\",\"enabled\":true}" :
                                          L"{\"type\":\"fullscreen\",\"enabled\":false}");
}

bool IsFourDigitPin(const std::wstring &pin) {
    if (pin.size() != 4) {
        return false;
    }
    for (wchar_t ch : pin) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }
    }
    return true;
}

std::wstring BuildPinMessageJson() {
    std::wstring json = L"{\"type\":\"pin\",\"pin\":";
    if (IsFourDigitPin(g_state.pin_value)) {
        json += L"\"";
        json += g_state.pin_value;
        json += L"\"";
    } else {
        json += L"null";
    }
    json += L",\"visible\":";
    json += g_state.pin_visible ? L"true}" : L"false}";
    return json;
}

void PostPinStateToWebView() {
    if (!g_state.pin_webview) {
        return;
    }
    std::wstring json = BuildPinMessageJson();
    g_state.pin_webview->PostWebMessageAsJson(json.c_str());
}

void PositionPinWindow() {
    if (!g_state.pin_hwnd) {
        return;
    }
    RECT bounds = GetPinWindowBounds();
    SetWindowPos(g_state.pin_hwnd, HWND_TOPMOST,
                 bounds.left, bounds.top,
                 bounds.right - bounds.left,
                 bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ResizeWebView();
}

void ApplyPinState(bool visible, const std::wstring &pin) {
    if (!pin.empty()) {
        g_state.pin_value = pin;
    }
    g_state.pin_visible = visible;

    if (g_state.pin_webview_controller) {
        g_state.pin_webview_controller->put_IsVisible(g_state.pin_visible ? TRUE : FALSE);
    }
    PostPinStateToWebView();

    if (!g_state.pin_hwnd) {
        return;
    }

    if (g_state.pin_visible) {
        PositionPinWindow();
        ShowWindow(g_state.pin_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_state.pin_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(g_state.pin_hwnd, kPinAutoHideTimer, kPinAutoHideMs, nullptr);
    } else {
        KillTimer(g_state.pin_hwnd, kPinAutoHideTimer);
        ShowWindow(g_state.pin_hwnd, SW_HIDE);
    }
}

void SetFullscreen(bool enabled) {
    if (!g_state.main_hwnd || g_state.fullscreen == enabled) {
        return;
    }

    if (enabled) {
        g_state.restore_style = GetWindowLongPtr(g_state.main_hwnd, GWL_STYLE);
        g_state.restore_ex_style = GetWindowLongPtr(g_state.main_hwnd, GWL_EXSTYLE);
        GetWindowRect(g_state.main_hwnd, &g_state.restore_rect);

        HMONITOR monitor = MonitorFromWindow(g_state.main_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info = {};
        monitor_info.cbSize = sizeof(monitor_info);
        GetMonitorInfo(monitor, &monitor_info);

        SetWindowLongPtr(g_state.main_hwnd, GWL_STYLE,
                         g_state.restore_style & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLongPtr(g_state.main_hwnd, GWL_EXSTYLE,
                         g_state.restore_ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                                      WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(g_state.main_hwnd, HWND_TOP,
                     monitor_info.rcMonitor.left, monitor_info.rcMonitor.top,
                     monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
                     monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtr(g_state.main_hwnd, GWL_STYLE, g_state.restore_style);
        SetWindowLongPtr(g_state.main_hwnd, GWL_EXSTYLE, g_state.restore_ex_style);
        SetWindowPos(g_state.main_hwnd, nullptr,
                     g_state.restore_rect.left, g_state.restore_rect.top,
                     g_state.restore_rect.right - g_state.restore_rect.left,
                     g_state.restore_rect.bottom - g_state.restore_rect.top,
                     SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    g_state.fullscreen = enabled;
    ResizeChildren(g_state.main_hwnd);
    PostFullscreenStateToWebView();
}

void ToggleFullscreen() {
    SetFullscreen(!g_state.fullscreen);
}

void DispatchAction(const char *action) {
    native_window_action_callback_t callback = nullptr;
    void *userdata = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        callback = g_state.action_callback;
        userdata = g_state.action_userdata;
    }
    if (callback) {
        callback(action, userdata);
    }
}

void HandleWebViewMessage(ICoreWebView2WebMessageReceivedEventArgs *args) {
    if (!args) {
        return;
    }

    LPWSTR message = nullptr;
    HRESULT hr = args->TryGetWebMessageAsString(&message);
    if (FAILED(hr) || !message) {
        return;
    }

    if (wcscmp(message, L"stop") == 0) {
        DispatchAction("stop");
    } else if (wcscmp(message, L"toggle-audio") == 0) {
        DispatchAction("toggle-audio");
    } else if (wcscmp(message, L"fullscreen") == 0) {
        ToggleFullscreen();
    }

    CoTaskMemFree(message);
}

const wchar_t *OverlayHtml() {
    return LR"html(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
:root {
  --overlay-border: rgba(255, 255, 255, 0.18);
}

* {
  box-sizing: border-box;
}

html,
body {
  width: 100%;
  height: 100%;
  margin: 0;
  background: transparent;
  overflow: hidden;
  color: #f5f7fa;
  font-family: "Segoe UI", Arial, sans-serif;
}

.toolbar {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  flex-direction: column;
  justify-content: center;
  gap: 8px;
  padding: 6px 10px;
  border-radius: 999px;
  background: linear-gradient(90deg, rgba(0, 0, 0, 0.25), rgba(0, 0, 0, 0.6));
  opacity: 0.92;
  transition: opacity 0.16s ease;
}

.toolbar:hover,
.toolbar.is-active {
  opacity: 1;
}

.tool-button {
  width: 36px;
  height: 36px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px solid rgba(255, 255, 255, 0.24);
  border-radius: 999px;
  background: rgba(0, 0, 0, 0.5);
  color: rgba(255, 255, 255, 0.8);
  cursor: pointer;
  padding: 0;
  transition:
    background 0.16s ease,
    border-color 0.16s ease,
    color 0.16s ease,
    opacity 0.16s ease,
    transform 0.16s ease;
}

.tool-button:hover {
  border-color: rgba(255, 255, 255, 0.3);
  background: rgba(255, 255, 255, 0.1);
  color: #fff;
}

.tool-button:active {
  transform: scale(0.95);
}

.tool-button.is-active {
  border-color: rgba(255, 255, 255, 0.4);
  background: rgba(255, 255, 255, 0.1);
  color: #fff;
}

.tool-button[hidden] {
  display: none;
}

.tool-button--close:hover {
  border-color: rgba(248, 113, 113, 0.5);
  background: rgba(239, 68, 68, 0.2);
  color: #f87171;
}

.tool-button__icon {
  width: 20px;
  height: 20px;
  display: block;
}

.mute-icon--muted,
.fullscreen-icon--window {
  display: none;
}

.tool-button.is-muted .mute-icon--muted,
.tool-button.is-fullscreen .fullscreen-icon--window {
  display: block;
}

.tool-button.is-muted .mute-icon--volume,
.tool-button.is-fullscreen .fullscreen-icon--full {
  display: none;
}

.sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  padding: 0;
  margin: -1px;
  overflow: hidden;
  clip: rect(0, 0, 0, 0);
  white-space: nowrap;
  border: 0;
}
</style>
</head>
<body>
  <nav class="toolbar is-active" aria-label="controls">
    <button id="muteToggleButton" class="tool-button" type="button" aria-label="audio">
      <svg class="tool-button__icon mute-icon--volume" viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="M4 9v6h4l5 4V5L8 9H4Z" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>
        <path d="M16 8.5c1.3 1.9 1.3 5.1 0 7" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
        <path d="M19 6c2.4 3.4 2.4 8.6 0 12" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
      </svg>
      <svg class="tool-button__icon mute-icon--muted" viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="M4 9v6h4l5 4V5L8 9H4Z" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>
        <path d="m18 9 4 4m0-4-4 4" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
      </svg>
      <span class="sr-only">Audio</span>
    </button>
    <button id="stopCastButton" class="tool-button tool-button--close" type="button" aria-label="stop casting">
      <svg class="tool-button__icon" viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="m6 6 12 12M18 6 6 18" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
      </svg>
      <span class="sr-only">Stop casting</span>
    </button>
    <button id="fullscreenToggleButton" class="tool-button" type="button" aria-label="fullscreen" hidden>
      <svg class="tool-button__icon fullscreen-icon--full" viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="M8 4H4v4M16 4h4v4M20 16v4h-4M4 16v4h4" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
      <svg class="tool-button__icon fullscreen-icon--window" viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="M9 4v5H4M15 4v5h5M20 15h-5v5M4 15h5v5" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
      <span class="sr-only">Fullscreen</span>
    </button>
  </nav>
  <script>
    const toolbar = document.querySelector(".toolbar");
    const muteToggleButton = document.getElementById("muteToggleButton");
    const stopCastButton = document.getElementById("stopCastButton");
    const fullscreenToggleButton = document.getElementById("fullscreenToggleButton");
    let muted = false;

    function setFullscreenState(enabled) {
      fullscreenToggleButton.classList.toggle("is-fullscreen", enabled);
      fullscreenToggleButton.setAttribute("aria-label", enabled ? "window" : "fullscreen");
    }

    function setMutedState(nextMuted) {
      muted = !!nextMuted;
      muteToggleButton.classList.toggle("is-muted", muted);
      muteToggleButton.classList.toggle("is-active", muted);
      toolbar.classList.toggle("is-active", muted);
      muteToggleButton.setAttribute("aria-label", muted ? "unmute" : "mute");
    }

    muteToggleButton.addEventListener("click", () => {
      setMutedState(!muted);
      chrome.webview.postMessage("toggle-audio");
    });

    stopCastButton.addEventListener("click", () => {
      chrome.webview.postMessage("stop");
    });

    fullscreenToggleButton.addEventListener("click", () => {
      chrome.webview.postMessage("fullscreen");
    });

    chrome.webview.addEventListener("message", (event) => {
      if (event.data && event.data.type === "fullscreen") {
        setFullscreenState(!!event.data.enabled);
      } else if (event.data && event.data.type === "muted") {
        setMutedState(!!event.data.enabled);
      }
    });
  </script>
</body>
</html>
)html";
}

const wchar_t *PinOverlayHtml() {
    return LR"html(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'unsafe-inline' 'self'; style-src 'unsafe-inline' 'self'" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>UxPlay PIN</title>
<style>
:root {
  --overlay-bg: rgba(12, 16, 22, 0.58);
  --overlay-border: rgba(255, 255, 255, 0.18);
  --text-main: #f5f7fa;
  --text-muted: rgba(245, 247, 250, 0.75);
}

* {
  box-sizing: border-box;
}

html,
body {
  width: 100%;
  height: 100%;
  margin: 0;
  overflow: hidden;
  background: transparent;
  color: var(--text-main);
  font-family: "Segoe UI", "PingFang SC", sans-serif;
}

body {
  display: grid;
  place-items: center;
}

.pin-overlay {
  display: flex;
  align-items: center;
  gap: 10px;
  min-height: 44px;
  padding: 10px 16px;
  border: 1px solid var(--overlay-border);
  border-radius: 999px;
  background: var(--overlay-bg);
  backdrop-filter: blur(14px);
  box-shadow: 0 8px 30px rgba(0, 0, 0, 0.28);
  pointer-events: none;
}

.pin-label {
  font-size: 12px;
  color: var(--text-muted);
  letter-spacing: 0.06em;
  text-transform: uppercase;
}

.pin-value {
  font-family: "Cascadia Mono", Consolas, monospace;
  font-size: 24px;
  line-height: 1;
  letter-spacing: 0.22em;
  font-variant-numeric: tabular-nums;
}
</style>
</head>
<body>
  <main class="pin-overlay" aria-live="polite">
    <span class="pin-label">PIN</span>
    <output id="pinValue" class="pin-value">----</output>
  </main>
  <script>
    const pinValue = document.getElementById("pinValue");

    function applyStatus(status) {
      const pin = typeof status?.pin === "string" && /^\d{4}$/.test(status.pin)
        ? status.pin
        : null;
      pinValue.textContent = pin || "----";
    }

    chrome.webview.addEventListener("message", (event) => {
      if (event.data && event.data.type === "pin") {
        applyStatus(event.data);
      }
    });
  </script>
</body>
</html>
)html";
}

class WebMessageReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    WebMessageReceivedHandler() : ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2WebMessageReceivedEventHandler)) {
            *object = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2WebMessageReceivedEventArgs *args) override {
        (void) sender;
        HandleWebViewMessage(args);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_;
};

class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    ControllerCreatedHandler() : ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code, ICoreWebView2Controller *controller) override {
        if (FAILED(error_code) || !controller) {
            LogHresult(LOGGER_WARNING, "failed to create WebView2 overlay controller", error_code);
            return S_OK;
        }

        g_state.webview_controller = controller;
        g_state.webview_controller->AddRef();
        g_state.webview_controller->put_IsVisible(TRUE);
        ResizeWebView();

        ICoreWebView2Controller2 *controller2 = nullptr;
        if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2,
                                                 reinterpret_cast<void **>(&controller2)))) {
            COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};
            controller2->put_DefaultBackgroundColor(transparent);
            controller2->Release();
        }

        HRESULT hr = controller->get_CoreWebView2(&g_state.webview);
        if (FAILED(hr) || !g_state.webview) {
            LogHresult(LOGGER_WARNING, "failed to get WebView2 core", hr);
            return S_OK;
        }

        WebMessageReceivedHandler *message_handler = new WebMessageReceivedHandler();
        hr = g_state.webview->add_WebMessageReceived(message_handler, &g_state.web_message_token);
        message_handler->Release();
        if (SUCCEEDED(hr)) {
            g_state.web_message_token_valid = true;
        } else {
            LogHresult(LOGGER_WARNING, "failed to subscribe WebView2 messages", hr);
        }

        hr = g_state.webview->NavigateToString(OverlayHtml());
        if (FAILED(hr)) {
            LogHresult(LOGGER_WARNING, "failed to load WebView2 overlay HTML", hr);
            return S_OK;
        }

        g_state.webview_ready = true;
        PostFullscreenStateToWebView();
        InvalidateRect(g_state.overlay_hwnd, nullptr, TRUE);
        Log(LOGGER_INFO, "created WebView2 HTML/CSS overlay controls");
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_;
};

class PinControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    PinControllerCreatedHandler() : ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code, ICoreWebView2Controller *controller) override {
        if (FAILED(error_code) || !controller) {
            LogHresult(LOGGER_WARNING, "failed to create WebView2 PIN controller", error_code);
            return S_OK;
        }

        g_state.pin_webview_controller = controller;
        g_state.pin_webview_controller->AddRef();
        g_state.pin_webview_controller->put_IsVisible(g_state.pin_visible ? TRUE : FALSE);
        ResizeWebView();

        ICoreWebView2Controller2 *controller2 = nullptr;
        if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2,
                                                 reinterpret_cast<void **>(&controller2)))) {
            COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};
            controller2->put_DefaultBackgroundColor(transparent);
            controller2->Release();
        }

        HRESULT hr = controller->get_CoreWebView2(&g_state.pin_webview);
        if (FAILED(hr) || !g_state.pin_webview) {
            LogHresult(LOGGER_WARNING, "failed to get WebView2 PIN core", hr);
            return S_OK;
        }

        hr = g_state.pin_webview->NavigateToString(PinOverlayHtml());
        if (FAILED(hr)) {
            LogHresult(LOGGER_WARNING, "failed to load WebView2 PIN HTML", hr);
            return S_OK;
        }

        g_state.pin_webview_ready = true;
        PostPinStateToWebView();
        Log(LOGGER_INFO, "created WebView2 HTML/CSS PIN overlay");
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_;
};

class EnvironmentCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvironmentCreatedHandler() : ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --ref_count_;
        if (ref == 0) {
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code, ICoreWebView2Environment *environment) override {
        if (FAILED(error_code) || !environment) {
            LogHresult(LOGGER_WARNING, "failed to create WebView2 environment", error_code);
            return S_OK;
        }

        g_state.webview_environment = environment;
        g_state.webview_environment->AddRef();

        ControllerCreatedHandler *handler = new ControllerCreatedHandler();
        HRESULT hr = environment->CreateCoreWebView2Controller(g_state.overlay_hwnd, handler);
        handler->Release();
        if (FAILED(hr)) {
            LogHresult(LOGGER_WARNING, "failed to start WebView2 controller creation", hr);
        }

        if (g_state.pin_hwnd) {
            PinControllerCreatedHandler *pin_handler = new PinControllerCreatedHandler();
            hr = environment->CreateCoreWebView2Controller(g_state.pin_hwnd, pin_handler);
            pin_handler->Release();
            if (FAILED(hr)) {
                LogHresult(LOGGER_WARNING, "failed to start WebView2 PIN controller creation", hr);
            }
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_;
};

void CreateWebViewOverlay() {
    if (!g_state.overlay_hwnd || !g_state.com_initialized) {
        return;
    }

    g_state.webview_loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!g_state.webview_loader) {
        Log(LOGGER_WARNING, "WebView2Loader.dll not found; falling back to Win32 overlay buttons");
        return;
    }

    typedef HRESULT (WINAPI *CreateEnvironmentWithOptionsFn)(
        PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    auto create_environment = reinterpret_cast<CreateEnvironmentWithOptionsFn>(
        GetProcAddress(g_state.webview_loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create_environment) {
        Log(LOGGER_WARNING, "CreateCoreWebView2EnvironmentWithOptions not found; falling back to Win32 overlay buttons");
        return;
    }

    std::wstring user_data = BuildWebViewUserDataFolder();
    EnvironmentCreatedHandler *handler = new EnvironmentCreatedHandler();
    HRESULT hr = create_environment(nullptr,
                                    user_data.empty() ? nullptr : user_data.c_str(),
                                    nullptr,
                                    handler);
    handler->Release();
    if (FAILED(hr)) {
        LogHresult(LOGGER_WARNING, "failed to start WebView2 environment creation", hr);
    }
}

void ReleaseWebViewOverlay() {
    if (g_state.webview && g_state.web_message_token_valid) {
        g_state.webview->remove_WebMessageReceived(g_state.web_message_token);
        g_state.web_message_token_valid = false;
    }

    if (g_state.webview_controller) {
        g_state.webview_controller->Close();
    }
    if (g_state.pin_webview_controller) {
        g_state.pin_webview_controller->Close();
    }

    ReleaseCom(&g_state.webview);
    ReleaseCom(&g_state.webview_controller);
    ReleaseCom(&g_state.pin_webview);
    ReleaseCom(&g_state.pin_webview_controller);
    ReleaseCom(&g_state.webview_environment);

    if (g_state.webview_loader) {
        FreeLibrary(g_state.webview_loader);
        g_state.webview_loader = nullptr;
    }
    g_state.webview_ready = false;
    g_state.pin_webview_ready = false;
}

void UninitializeWindowThreadCom() {
    if (g_state.com_initialized) {
        CoUninitialize();
        g_state.com_initialized = false;
    }
}

void PaintButton(HDC hdc, const RECT &rect, const wchar_t *label, bool hot) {
    HBRUSH brush = CreateSolidBrush(hot ? RGB(46, 46, 46) : RGB(30, 30, 30));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(96, 96, 96));
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    int radius = std::max(1, static_cast<int>(rect.bottom - rect.top) / 2);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, label, -1, const_cast<RECT *>(&rect),
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    static int hot_button = -1;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        if (g_state.webview_ready) {
            return HTTRANSPARENT;
        }
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        if (PointInRect(GetButtonRect(hwnd, 0), pt) ||
            PointInRect(GetButtonRect(hwnd, 1), pt)) {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_MOUSEMOVE: {
        if (g_state.webview_ready) {
            return 0;
        }
        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);

        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int next_hot = PointInRect(GetButtonRect(hwnd, 0), pt) ? 0 :
                       (PointInRect(GetButtonRect(hwnd, 1), pt) ? 1 : -1);
        if (hot_button != next_hot) {
            hot_button = next_hot;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_state.webview_ready) {
            return 0;
        }
        if (hot_button != -1) {
            hot_button = -1;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (g_state.webview_ready) {
            return 0;
        }
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (PointInRect(GetButtonRect(hwnd, 0), pt)) {
            DispatchAction("toggle-audio");
        } else if (PointInRect(GetButtonRect(hwnd, 1), pt)) {
            DispatchAction("stop");
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPinAutoHideTimer) {
            ApplyPinState(false, L"");
            return 0;
        }
        return DefWindowProc(hwnd, msg, wparam, lparam);
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!g_state.webview_ready) {
            PaintButton(hdc, GetButtonRect(hwnd, 0), L"Audio", hot_button == 0);
            PaintButton(hdc, GetButtonRect(hwnd, 1), L"Stop", hot_button == 1);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

LRESULT CALLBACK PinWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_TIMER:
        if (wparam == kPinAutoHideTimer) {
            ApplyPinState(false, L"");
            return 0;
        }
        return DefWindowProc(hwnd, msg, wparam, lparam);
    case WM_CLOSE:
        ApplyPinState(false, L"");
        return 0;
    case WM_DESTROY:
        if (g_state.pin_hwnd == hwnd) {
            g_state.pin_hwnd = nullptr;
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_SIZE:
        ResizeChildren(hwnd);
        return 0;
    case WM_DPICHANGED: {
        g_state.dpi = HIWORD(wparam);
        const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ResizeChildren(hwnd);
        PositionPinWindow();
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case kNativeWindowDestroyMessage:
        DestroyWindow(hwnd);
        return 0;
    case kNativeWindowPinMessage: {
        PinUpdate *update = reinterpret_cast<PinUpdate *>(lparam);
        if (update) {
            bool next_visible = update->show || g_state.pin_visible;
            ApplyPinState(next_visible, update->pin);
            delete update;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F11 || (wparam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
            ToggleFullscreen();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (g_state.pin_hwnd) {
            DestroyWindow(g_state.pin_hwnd);
            g_state.pin_hwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

bool RegisterWindowClasses(HINSTANCE instance) {
    WNDCLASSEXW main_class = {};
    main_class.cbSize = sizeof(main_class);
    main_class.lpfnWndProc = MainWndProc;
    main_class.hInstance = instance;
    main_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    main_class.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&main_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW overlay_class = {};
    overlay_class.cbSize = sizeof(overlay_class);
    overlay_class.lpfnWndProc = OverlayWndProc;
    overlay_class.hInstance = instance;
    overlay_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    overlay_class.hbrBackground = nullptr;
    overlay_class.lpszClassName = kOverlayWindowClass;
    if (!RegisterClassExW(&overlay_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW pin_class = {};
    pin_class.cbSize = sizeof(pin_class);
    pin_class.lpfnWndProc = PinWndProc;
    pin_class.hInstance = instance;
    pin_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pin_class.hbrBackground = nullptr;
    pin_class.lpszClassName = kPinWindowClass;
    if (!RegisterClassExW(&pin_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    return true;
}

HWND CreatePinWindow() {
    RECT bounds = GetPinWindowBounds();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                    WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                kPinWindowClass, nullptr, WS_POPUP,
                                bounds.left, bounds.top,
                                bounds.right - bounds.left,
                                bounds.bottom - bounds.top,
                                nullptr, nullptr, g_state.instance, nullptr);
    if (hwnd) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    }
    return hwnd;
}

void NotifyReady(bool failed) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.create_failed = failed;
    g_state.ready = true;
    g_state.ready_cv.notify_all();
}

void WindowThread() {
    EnableProcessDpiAwareness();

    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_state.com_initialized = SUCCEEDED(co_hr);
    if (FAILED(co_hr)) {
        LogHresult(LOGGER_WARNING, "failed to initialize COM for WebView2 overlay", co_hr);
    }

    g_state.instance = GetModuleHandleW(nullptr);
    if (!RegisterWindowClasses(g_state.instance)) {
        NotifyReady(true);
        UninitializeWindowThreadCom();
        return;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    DWORD ex_style = 0;
    g_state.dpi = GetSystemDpiValue();
    RECT rect = {0, 0,
                 ScaleForDpi(kInitialWindowWidth, g_state.dpi),
                 ScaleForDpi(kInitialWindowHeight, g_state.dpi)};
    rect = AdjustWindowRectForDpi(rect, style, ex_style, g_state.dpi);

    g_state.main_hwnd = CreateWindowExW(ex_style, kMainWindowClass, g_state.title.c_str(), style,
                                        CW_USEDEFAULT, CW_USEDEFAULT,
                                        rect.right - rect.left, rect.bottom - rect.top,
                                        nullptr, nullptr, g_state.instance, nullptr);
    if (!g_state.main_hwnd) {
        NotifyReady(true);
        UninitializeWindowThreadCom();
        return;
    }

    g_state.video_hwnd = CreateWindowExW(0, L"STATIC", nullptr,
                                         WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
                                         0, 0,
                                         ScaleForDpi(kInitialWindowWidth, g_state.dpi),
                                         ScaleForDpi(kInitialWindowHeight, g_state.dpi),
                                         g_state.main_hwnd, nullptr, g_state.instance, nullptr);
    g_state.overlay_hwnd = CreateWindowExW(WS_EX_TRANSPARENT, kOverlayWindowClass, nullptr,
                                           WS_CHILD | WS_VISIBLE,
                                           0, 0,
                                           ScaleForDpi(kInitialWindowWidth, g_state.dpi),
                                           ScaleForDpi(kInitialWindowHeight, g_state.dpi),
                                           g_state.main_hwnd, nullptr, g_state.instance, nullptr);

    if (!g_state.video_hwnd || !g_state.overlay_hwnd) {
        if (g_state.pin_hwnd) {
            DestroyWindow(g_state.pin_hwnd);
            g_state.pin_hwnd = nullptr;
        }
        DestroyWindow(g_state.main_hwnd);
        g_state.main_hwnd = nullptr;
        g_state.video_hwnd = nullptr;
        g_state.overlay_hwnd = nullptr;
        NotifyReady(true);
        UninitializeWindowThreadCom();
        return;
    }

    g_state.pin_hwnd = CreatePinWindow();
    if (!g_state.pin_hwnd) {
        Log(LOGGER_WARNING, "failed to create native Windows PIN popup window");
    }

    CreateWebViewOverlay();
    ResizeChildren(g_state.main_hwnd);
    if (g_state.fullscreen) {
        bool requested_fullscreen = g_state.fullscreen;
        g_state.fullscreen = false;
        SetFullscreen(requested_fullscreen);
    }

    NotifyReady(false);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseWebViewOverlay();
    UninitializeWindowThreadCom();

    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.running = false;
    g_state.main_hwnd = nullptr;
    g_state.video_hwnd = nullptr;
    g_state.overlay_hwnd = nullptr;
    g_state.pin_hwnd = nullptr;
}

} // namespace

extern "C" bool native_window_should_embed(const char *videosink, const char *videosink_options,
                                            bool hls_video, uint32_t shared_texture_target_pid) {
    (void) videosink_options;
    return !hls_video && shared_texture_target_pid == 0 &&
           videosink && strcmp(videosink, "d3d11videosink") == 0;
}

extern "C" bool native_window_create(logger_t *logger, const char *title, bool fullscreen) {
    std::unique_lock<std::mutex> lock(g_state.mutex);
    if (g_state.running && g_state.video_hwnd) {
        return true;
    }

    g_state.logger = logger;
    g_state.title = ToWide(title);
    g_state.fullscreen = fullscreen;
    g_state.ready = false;
    g_state.create_failed = false;
    g_state.running = true;

    g_state.window_thread = std::thread(WindowThread);
    g_state.ready_cv.wait(lock, [] { return g_state.ready; });
    bool ok = !g_state.create_failed && g_state.video_hwnd != nullptr;
    if (!ok) {
        g_state.running = false;
        lock.unlock();
        if (g_state.window_thread.joinable()) {
            g_state.window_thread.join();
        }
        Log(LOGGER_WARNING, "failed to create native Windows video window");
        return false;
    }
    Log(LOGGER_INFO, "created native Windows video window for d3d11videosink");
    return true;
}

extern "C" void native_window_set_action_callback(native_window_action_callback_t callback, void *userdata) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.action_callback = callback;
    g_state.action_userdata = userdata;
}

extern "C" uintptr_t native_window_get_video_handle(void) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return reinterpret_cast<uintptr_t>(g_state.video_hwnd);
}

extern "C" void native_window_show(void) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        hwnd = g_state.main_hwnd;
    }
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

extern "C" void native_window_set_pin(const char *pin, bool show) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        hwnd = g_state.main_hwnd;
    }
    if (!hwnd) {
        return;
    }

    PinUpdate *update = new PinUpdate();
    update->show = show;
    if (pin && strlen(pin) == 4) {
        bool valid = true;
        for (int i = 0; i < 4; i++) {
            if (pin[i] < '0' || pin[i] > '9') {
                valid = false;
                break;
            }
            update->pin[i] = static_cast<wchar_t>(pin[i]);
        }
        if (!valid) {
            update->pin[0] = L'\0';
        }
    }

    if (!PostMessageW(hwnd, kNativeWindowPinMessage, 0, reinterpret_cast<LPARAM>(update))) {
        delete update;
    }
}

extern "C" void native_window_destroy(void) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        hwnd = g_state.main_hwnd;
    }
    if (hwnd) {
        PostMessageW(hwnd, kNativeWindowDestroyMessage, 0, 0);
    }
    if (g_state.window_thread.joinable() &&
        g_state.window_thread.get_id() != std::this_thread::get_id()) {
        g_state.window_thread.join();
    }
}

#else

extern "C" bool native_window_should_embed(const char *videosink, const char *videosink_options,
                                            bool hls_video, uint32_t shared_texture_target_pid) {
    (void) videosink;
    (void) videosink_options;
    (void) hls_video;
    (void) shared_texture_target_pid;
    return false;
}

extern "C" bool native_window_create(logger_t *logger, const char *title, bool fullscreen) {
    (void) logger;
    (void) title;
    (void) fullscreen;
    return false;
}

extern "C" void native_window_set_action_callback(native_window_action_callback_t callback, void *userdata) {
    (void) callback;
    (void) userdata;
}

extern "C" uintptr_t native_window_get_video_handle(void) {
    return 0;
}

extern "C" void native_window_show(void) {
}

extern "C" void native_window_set_pin(const char *pin, bool show) {
    (void) pin;
    (void) show;
}

extern "C" void native_window_destroy(void) {
}

#endif
