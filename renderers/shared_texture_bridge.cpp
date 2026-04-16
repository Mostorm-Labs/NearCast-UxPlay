#include "shared_texture_bridge.h"

#include <gst/gst.h>

#if defined(_WIN32) && defined(UXPLAY_HAS_SHARED_TEXTURE_BRIDGE)

#include <gst/d3d11/gstd3d11memory.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kInvalidPoolIndex = std::numeric_limits<size_t>::max();
constexpr size_t kFallbackSharedTexturePoolSize = 16;
constexpr auto kFallbackSlotLeaseTimeout = std::chrono::milliseconds(1200);

struct FrameRecord {
  GstSample *sample = nullptr;
  size_t shared_texture_pool_index = kInvalidPoolIndex;
  size_t shared_texture_slot_index = kInvalidPoolIndex;

  FrameRecord() = default;
  FrameRecord(GstSample *sample_in, size_t pool_index_in, size_t slot_index_in)
      : sample(sample_in),
        shared_texture_pool_index(pool_index_in),
        shared_texture_slot_index(slot_index_in) {}
};

struct SharedTexturePoolSlot {
  ID3D11Texture2D *texture = nullptr;
  HANDLE shared_handle = nullptr;
  bool in_use = false;
  guint64 owner_frame_id = 0;
  std::chrono::steady_clock::time_point lease_started_at{};
};

struct SharedTexturePool {
  ID3D11Device *device = nullptr;
  D3D11_TEXTURE2D_DESC desc = {};
  std::vector<SharedTexturePoolSlot> slots;
};

enum class SharedHandleStatus {
  kSuccess,
  kDropFrame,
  kFailure,
};

template <typename T>
void SafeRelease(T *&ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

std::string FormatHRESULT(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return stream.str();
}

void CloseHandleIfValid(HANDLE &handle) {
  if (handle) {
    CloseHandle(handle);
    handle = nullptr;
  }
}

void ReleaseSharedTexturePoolSlot(SharedTexturePoolSlot &slot) {
  SafeRelease(slot.texture);
  CloseHandleIfValid(slot.shared_handle);
  slot.in_use = false;
  slot.owner_frame_id = 0;
  slot.lease_started_at = {};
}

void ReleaseSharedTexturePool(SharedTexturePool &pool) {
  for (auto &slot : pool.slots) {
    ReleaseSharedTexturePoolSlot(slot);
  }
  pool.slots.clear();
  SafeRelease(pool.device);
  pool.desc = {};
}

}  // namespace

struct shared_texture_bridge_s {
  shared_texture_bridge_s(logger_t *logger_in, uint32_t target_pid_in)
      : logger(logger_in), target_pid(target_pid_in) {}

  ~shared_texture_bridge_s() {
    Stop();
  }

  bool Start() {
    target_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(target_pid));
    if (!target_process) {
      Log(LOGGER_ERR, "shared texture bridge failed to open target process pid=%u", target_pid);
      return false;
    }

    command_thread = std::thread(&shared_texture_bridge_s::CommandLoop, this);
    std::cout << "READY" << std::endl;
    return true;
  }

  void Stop() {
    bool expected = false;
    if (!stop_started.compare_exchange_strong(expected, true)) {
      return;
    }

    stopping = true;
    SetSessionActive(false, "bridge-stop");

    if (command_thread.joinable()) {
      command_thread.join();
    }

    ReleaseAllFrames();

    for (auto &pool : shared_texture_pools) {
      ReleaseSharedTexturePool(pool);
    }
    shared_texture_pools.clear();

    if (target_process) {
      CloseHandle(target_process);
      target_process = nullptr;
    }
  }

  bool IsActive() const {
    return target_process != nullptr && exporting_enabled.load(std::memory_order_relaxed);
  }

  GstFlowReturn OnNewSample(GstAppSink *sink) {
    if (!sink) {
      return GST_FLOW_OK;
    }

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
      return GST_FLOW_OK;
    }

    if (!IsActive()) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
      gst_sample_unref(sample);
      Log(LOGGER_ERR, "shared texture sample missing buffer or caps");
      return GST_FLOW_OK;
    }

    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_is_d3d11_memory(memory)) {
      gst_sample_unref(sample);
      Log(LOGGER_ERR, "shared texture sample is not backed by GstD3D11Memory");
      return GST_FLOW_OK;
    }

    auto *d3d11_memory = GST_D3D11_MEMORY_CAST(memory);
    D3D11_TEXTURE2D_DESC desc = {};
    if (!gst_d3d11_memory_get_texture_desc(d3d11_memory, &desc)) {
      gst_sample_unref(sample);
      Log(LOGGER_ERR, "shared texture export failed to inspect D3D11 texture description");
      return GST_FLOW_OK;
    }

    HANDLE local_handle = nullptr;
    bool close_local_handle = true;
    size_t shared_texture_pool_index = kInvalidPoolIndex;
    size_t shared_texture_slot_index = kInvalidPoolIndex;

    if (!gst_d3d11_memory_get_nt_handle(d3d11_memory, &local_handle) || !local_handle) {
      const SharedHandleStatus status =
          CreateShareableTextureHandle(d3d11_memory, buffer, caps, desc, &local_handle,
                                       &close_local_handle, &shared_texture_pool_index,
                                       &shared_texture_slot_index);
      if (status == SharedHandleStatus::kDropFrame) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }

      if (status != SharedHandleStatus::kSuccess || !local_handle) {
        gst_sample_unref(sample);
        Log(LOGGER_ERR, "shared texture export failed to create shareable handle, dropping frame");
        return GST_FLOW_OK;
      }

      EmitInfoOnce(fallback_info_logged,
                   "shared texture export is using a bridge-owned copy because upstream textures are not shareable");
    } else {
      EmitInfoOnce(direct_info_logged, "shared texture export is using native GstD3D11Memory NT handles");
    }

    HANDLE remote_handle = nullptr;
    const BOOL duplicated =
        DuplicateHandle(GetCurrentProcess(), local_handle, target_process, &remote_handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    if (close_local_handle) {
      CloseHandle(local_handle);
    }

    if (!duplicated || !remote_handle) {
      gst_sample_unref(sample);
      ReleaseSharedTextureSlot(shared_texture_pool_index, shared_texture_slot_index);
      if (!duplicate_failure_logged.exchange(true, std::memory_order_relaxed)) {
        Log(LOGGER_ERR, "shared texture export failed to duplicate handle into pid=%u", target_pid);
      }
      exporting_enabled.store(false, std::memory_order_relaxed);
      SetSessionActive(false, "duplicate-handle-failed");
      ReleaseAllFrames();
      return GST_FLOW_OK;
    }

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
      pts = gst_util_get_timestamp();
    }

    const guint64 frame_id = next_frame_id.fetch_add(1, std::memory_order_relaxed);
    const bool uses_fallback_pool = shared_texture_pool_index != kInvalidPoolIndex;
    GstSample *sample_to_hold = sample;
    if (uses_fallback_pool) {
      // Fallback frames are copied into bridge-owned textures, so holding GstSample
      // until RELEASE is unnecessary and can amplify pipeline pressure.
      gst_sample_unref(sample);
      sample_to_hold = nullptr;
    }

    {
      std::lock_guard<std::mutex> lock(frame_mutex);
      if (shared_texture_pool_index != kInvalidPoolIndex &&
          shared_texture_pool_index < shared_texture_pools.size()) {
        SharedTexturePool &pool = shared_texture_pools[shared_texture_pool_index];
        if (shared_texture_slot_index < pool.slots.size()) {
          pool.slots[shared_texture_slot_index].owner_frame_id = frame_id;
        }
      }
      frames.emplace(frame_id,
                     FrameRecord{sample_to_hold, shared_texture_pool_index, shared_texture_slot_index});
    }

    std::ostringstream line;
    line << "FRAME\t" << frame_id << '\t' << desc.Width << '\t' << desc.Height << '\t'
         << (pts / GST_USECOND) << '\t' << "0x" << std::hex
         << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(remote_handle));
    std::cout << line.str() << std::endl;
    SetSessionActive(true, "frame-exported");

    const guint64 exported = exported_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (exported <= 5 || exported % 120 == 0) {
      size_t in_flight = 0;
      {
        std::lock_guard<std::mutex> lock(frame_mutex);
        in_flight = frames.size();
      }
      Log(LOGGER_DEBUG, "shared texture export progress: exported=%llu in_flight=%zu released=%llu",
          static_cast<unsigned long long>(exported), in_flight,
          static_cast<unsigned long long>(released_frames.load(std::memory_order_relaxed)));
    }
    return GST_FLOW_OK;
  }

  void CommandLoop() {
    while (!stopping.load(std::memory_order_relaxed)) {
      HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
      if (!stdin_handle || stdin_handle == INVALID_HANDLE_VALUE) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        continue;
      }

      DWORD bytes_available = 0;
      if (!PeekNamedPipe(stdin_handle, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        continue;
      }

      if (bytes_available == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      std::string line;
      while (!stopping.load(std::memory_order_relaxed) && std::getline(std::cin, line)) {
        HandleCommand(line);
        if (!PeekNamedPipe(stdin_handle, nullptr, 0, nullptr, &bytes_available, nullptr) ||
            bytes_available == 0) {
          break;
        }
      }

      if (std::cin.fail() && !std::cin.eof()) {
        std::cin.clear();
      }
    }
  }

  void HandleCommand(const std::string &line) {
    if (line == "STOP") {
      exporting_enabled.store(false, std::memory_order_relaxed);
      SetSessionActive(false, "stop-command");
      ReleaseAllFrames();
      return;
    }

    constexpr char kReleasePrefix[] = "RELEASE\t";
    if (line.rfind(kReleasePrefix, 0) != 0) {
      return;
    }

    const std::string id_text = line.substr(sizeof(kReleasePrefix) - 1);
    char *end = nullptr;
    const unsigned long long id = std::strtoull(id_text.c_str(), &end, 10);
    if (end && *end == '\0') {
      ReleaseFrame(static_cast<guint64>(id));
    }
  }

  void ReleaseAllFrames() {
    std::unordered_map<guint64, FrameRecord> released_frames;
    {
      std::lock_guard<std::mutex> lock(frame_mutex);
      released_frames.swap(frames);
    }

    for (auto &entry : released_frames) {
      if (entry.second.sample) {
        gst_sample_unref(entry.second.sample);
      }
      ReleaseSharedTextureSlot(entry.second.shared_texture_pool_index,
                               entry.second.shared_texture_slot_index);
    }
  }

  void ReleaseFrame(guint64 frame_id) {
    GstSample *sample = nullptr;
    size_t shared_texture_pool_index = kInvalidPoolIndex;
    size_t shared_texture_slot_index = kInvalidPoolIndex;
    size_t frames_in_flight = 0;
    {
      std::lock_guard<std::mutex> lock(frame_mutex);
      const auto it = frames.find(frame_id);
      if (it == frames.end()) {
        return;
      }
      sample = it->second.sample;
      shared_texture_pool_index = it->second.shared_texture_pool_index;
      shared_texture_slot_index = it->second.shared_texture_slot_index;
      frames.erase(it);
      frames_in_flight = frames.size();
    }

    if (sample) {
      gst_sample_unref(sample);
    }
    ReleaseSharedTextureSlot(shared_texture_pool_index, shared_texture_slot_index);

    const guint64 released = released_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (released <= 5 || released % 120 == 0) {
      Log(LOGGER_DEBUG,
          "shared texture release progress: released=%llu in_flight=%zu exported=%llu",
          static_cast<unsigned long long>(released), frames_in_flight,
          static_cast<unsigned long long>(exported_frames.load(std::memory_order_relaxed)));
    }
  }

  void Log(int level, const char *format, ...) const {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (logger) {
      logger_log(logger, level, "%s", buffer);
      return;
    }

    std::cerr << buffer << std::endl;
  }

  void EmitInfoOnce(std::atomic<bool> &flag, const char *message) const {
    bool expected = false;
    if (flag.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      Log(LOGGER_INFO, "%s", message);
    }
  }

  void SetSessionActive(bool active, const char *reason) {
    const int next_state = active ? 1 : 0;
    const int previous_state = session_active_state.exchange(next_state, std::memory_order_relaxed);
    if (previous_state == next_state) {
      return;
    }

    EmitSessionStateLine(active, reason);
  }

  void EmitSessionStateLine(bool active, const char *reason) const {
    std::ostringstream line;
    line << "SESSION\t" << (active ? "ACTIVE" : "INACTIVE") << '\t';
    if (reason && reason[0]) {
      line << reason;
    } else {
      line << "unspecified";
    }
    std::cout << line.str() << std::endl;
  }

  bool DescsMatch(const D3D11_TEXTURE2D_DESC &left, const D3D11_TEXTURE2D_DESC &right) const {
    return left.Width == right.Width && left.Height == right.Height &&
           left.Format == right.Format;
  }

  bool InitializeSharedTexturePool(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &source_desc,
                                   SharedTexturePool *pool) {
    if (!device || !pool) {
      return false;
    }

    D3D11_TEXTURE2D_DESC shared_desc = {};
    shared_desc.Width = source_desc.Width;
    shared_desc.Height = source_desc.Height;
    shared_desc.MipLevels = 1;
    shared_desc.ArraySize = 1;
    shared_desc.Format = source_desc.Format;
    shared_desc.SampleDesc.Count = 1;
    shared_desc.SampleDesc.Quality = 0;
    shared_desc.Usage = D3D11_USAGE_DEFAULT;
    shared_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    shared_desc.CPUAccessFlags = 0;
    shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                            D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    SharedTexturePool created_pool;
    device->AddRef();
    created_pool.device = device;
    created_pool.desc = shared_desc;
    created_pool.slots.resize(kFallbackSharedTexturePoolSize);

    for (size_t slot_index = 0; slot_index < created_pool.slots.size(); ++slot_index) {
      SharedTexturePoolSlot &slot = created_pool.slots[slot_index];
      HRESULT hr = device->CreateTexture2D(&shared_desc, nullptr, &slot.texture);
      if (FAILED(hr) || !slot.texture) {
        Log(LOGGER_ERR,
            "shared texture export failed to create pooled texture: %s slot=%zu size=%ux%u format=%u",
            FormatHRESULT(hr).c_str(), slot_index, shared_desc.Width, shared_desc.Height,
            static_cast<unsigned>(shared_desc.Format));
        ReleaseSharedTexturePool(created_pool);
        return false;
      }

      IDXGIResource1 *dxgi_resource = nullptr;
      hr = slot.texture->QueryInterface(IID_IDXGIResource1,
                                        reinterpret_cast<void **>(&dxgi_resource));
      if (FAILED(hr) || !dxgi_resource) {
        Log(LOGGER_ERR, "shared texture export failed to query IDXGIResource1: %s",
            FormatHRESULT(hr).c_str());
        ReleaseSharedTexturePool(created_pool);
        return false;
      }

      hr = dxgi_resource->CreateSharedHandle(nullptr,
                                             DXGI_SHARED_RESOURCE_READ |
                                                 DXGI_SHARED_RESOURCE_WRITE,
                                             nullptr, &slot.shared_handle);
      SafeRelease(dxgi_resource);
      if (FAILED(hr) || !slot.shared_handle) {
        Log(LOGGER_ERR, "shared texture export failed to create pooled NT handle: %s",
            FormatHRESULT(hr).c_str());
        ReleaseSharedTexturePool(created_pool);
        return false;
      }
    }

    *pool = std::move(created_pool);
    return true;
  }

  SharedHandleStatus AcquireSharedTextureSlot(ID3D11Device *device,
                                              const D3D11_TEXTURE2D_DESC &source_desc,
                                              size_t *pool_index_out, size_t *slot_index_out,
                                              HANDLE *shared_handle_out,
                                              ID3D11Texture2D **shared_texture_out) {
    if (!device || !pool_index_out || !slot_index_out || !shared_handle_out ||
        !shared_texture_out) {
      return SharedHandleStatus::kFailure;
    }

    std::lock_guard<std::mutex> lock(frame_mutex);

    size_t pool_index = kInvalidPoolIndex;
    for (size_t i = 0; i < shared_texture_pools.size(); ++i) {
      const SharedTexturePool &pool = shared_texture_pools[i];
      if (pool.device == device && DescsMatch(pool.desc, source_desc)) {
        pool_index = i;
        break;
      }
    }

    if (pool_index == kInvalidPoolIndex) {
      shared_texture_pools.emplace_back();
      pool_index = shared_texture_pools.size() - 1;
      if (!InitializeSharedTexturePool(device, source_desc, &shared_texture_pools[pool_index])) {
        shared_texture_pools.pop_back();
        return SharedHandleStatus::kFailure;
      }
    }

    SharedTexturePool &pool = shared_texture_pools[pool_index];
    const auto acquire_free_slot = [&](const std::chrono::steady_clock::time_point now) -> bool {
      for (size_t slot_index = 0; slot_index < pool.slots.size(); ++slot_index) {
        SharedTexturePoolSlot &slot = pool.slots[slot_index];
        if (slot.in_use) {
          continue;
        }

        slot.in_use = true;
        slot.owner_frame_id = 0;
        slot.lease_started_at = now;
        *pool_index_out = pool_index;
        *slot_index_out = slot_index;
        *shared_handle_out = slot.shared_handle;
        *shared_texture_out = slot.texture;
        return true;
      }
      return false;
    };

    const auto now = std::chrono::steady_clock::now();
    if (acquire_free_slot(now)) {
      return SharedHandleStatus::kSuccess;
    }

    unsigned reclaimed = 0;
    for (size_t slot_index = 0; slot_index < pool.slots.size(); ++slot_index) {
      SharedTexturePoolSlot &slot = pool.slots[slot_index];
      if (!slot.in_use || slot.owner_frame_id == 0) {
        continue;
      }
      if (now - slot.lease_started_at <= kFallbackSlotLeaseTimeout) {
        continue;
      }

      const auto frame_it = frames.find(slot.owner_frame_id);
      if (frame_it != frames.end()) {
        if (frame_it->second.sample) {
          gst_sample_unref(frame_it->second.sample);
        }
        frames.erase(frame_it);
      }
      slot.in_use = false;
      slot.owner_frame_id = 0;
      slot.lease_started_at = {};
      reclaimed += 1;
    }

    if (reclaimed > 0) {
      const unsigned total_reclaimed =
          reclaimed_pool_slots.fetch_add(reclaimed, std::memory_order_relaxed) + reclaimed;
      Log(LOGGER_WARNING,
          "shared texture export force-reclaimed %u stale pooled textures (timeout=%llums, total=%u)",
          reclaimed,
          static_cast<unsigned long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(kFallbackSlotLeaseTimeout)
                  .count()),
          total_reclaimed);
      if (acquire_free_slot(std::chrono::steady_clock::now())) {
        return SharedHandleStatus::kSuccess;
      }
    }

    // Preview-first policy: if everything is still busy, preempt the oldest slot
    // so export keeps moving instead of stalling on RELEASE delays.
    size_t oldest_slot_index = kInvalidPoolIndex;
    auto oldest_lease_started_at = now;
    for (size_t slot_index = 0; slot_index < pool.slots.size(); ++slot_index) {
      const SharedTexturePoolSlot &slot = pool.slots[slot_index];
      if (!slot.in_use) {
        continue;
      }
      if (oldest_slot_index == kInvalidPoolIndex || slot.lease_started_at < oldest_lease_started_at) {
        oldest_slot_index = slot_index;
        oldest_lease_started_at = slot.lease_started_at;
      }
    }

    if (oldest_slot_index != kInvalidPoolIndex) {
      SharedTexturePoolSlot &slot = pool.slots[oldest_slot_index];
      if (slot.owner_frame_id != 0) {
        const auto frame_it = frames.find(slot.owner_frame_id);
        if (frame_it != frames.end()) {
          if (frame_it->second.sample) {
            gst_sample_unref(frame_it->second.sample);
          }
          frames.erase(frame_it);
        }
      }
      slot.in_use = false;
      slot.owner_frame_id = 0;
      slot.lease_started_at = {};

      const unsigned total_preempted =
          preempted_pool_slots.fetch_add(1, std::memory_order_relaxed) + 1;
      if (total_preempted <= 5 || total_preempted % 60 == 0) {
        Log(LOGGER_WARNING,
            "shared texture export preempted oldest pooled texture slot to keep realtime preview (pool=%zu total=%u)",
            pool.slots.size(), total_preempted);
      }

      if (acquire_free_slot(std::chrono::steady_clock::now())) {
        return SharedHandleStatus::kSuccess;
      }
    }

    const unsigned dropped = dropped_pool_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped <= 5 || dropped % 60 == 0) {
      Log(LOGGER_WARNING,
          "shared texture export dropped a frame because all pooled textures are busy (pool=%zu dropped=%u)",
          pool.slots.size(), dropped);
    }
    return SharedHandleStatus::kDropFrame;
  }

  void ReleaseSharedTextureSlot(size_t pool_index, size_t slot_index) {
    if (pool_index == kInvalidPoolIndex || slot_index == kInvalidPoolIndex) {
      return;
    }

    std::lock_guard<std::mutex> lock(frame_mutex);
    if (pool_index >= shared_texture_pools.size()) {
      return;
    }

    SharedTexturePool &pool = shared_texture_pools[pool_index];
    if (slot_index >= pool.slots.size()) {
      return;
    }

    pool.slots[slot_index].in_use = false;
    pool.slots[slot_index].owner_frame_id = 0;
    pool.slots[slot_index].lease_started_at = {};
  }

  SharedHandleStatus CreateShareableTextureHandle(
      GstD3D11Memory *memory, GstBuffer *buffer, GstCaps *caps,
      const D3D11_TEXTURE2D_DESC &source_desc, HANDLE *shared_handle,
      bool *close_shared_handle, size_t *shared_texture_pool_index_out,
      size_t *shared_texture_slot_index_out) {
    (void) buffer;
    (void) caps;
    if (!shared_handle || !close_shared_handle || !shared_texture_pool_index_out ||
        !shared_texture_slot_index_out) {
      return SharedHandleStatus::kFailure;
    }

    *shared_handle = nullptr;
    *close_shared_handle = true;
    *shared_texture_pool_index_out = kInvalidPoolIndex;
    *shared_texture_slot_index_out = kInvalidPoolIndex;

    ID3D11Resource *source_resource = gst_d3d11_memory_get_resource_handle(memory);
    if (!source_resource) {
      Log(LOGGER_ERR, "shared texture export did not receive an ID3D11Resource");
      return SharedHandleStatus::kFailure;
    }

    ID3D11Texture2D *source_texture = nullptr;
    HRESULT hr = source_resource->QueryInterface(IID_ID3D11Texture2D,
                                                 reinterpret_cast<void **>(&source_texture));
    if (FAILED(hr) || !source_texture) {
      Log(LOGGER_ERR, "shared texture export failed to query source texture: %s",
          FormatHRESULT(hr).c_str());
      return SharedHandleStatus::kFailure;
    }

    ID3D11Device *device = nullptr;
    source_texture->GetDevice(&device);
    if (!device) {
      SafeRelease(source_texture);
      Log(LOGGER_ERR, "shared texture export failed to get D3D11 device");
      return SharedHandleStatus::kFailure;
    }

    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
      SafeRelease(device);
      SafeRelease(source_texture);
      Log(LOGGER_ERR, "shared texture export failed to get D3D11 device context");
      return SharedHandleStatus::kFailure;
    }

    ID3D11Texture2D *shared_texture = nullptr;
    const SharedHandleStatus acquire_status =
        AcquireSharedTextureSlot(device, source_desc, shared_texture_pool_index_out,
                                 shared_texture_slot_index_out, shared_handle, &shared_texture);
    if (acquire_status != SharedHandleStatus::kSuccess) {
      SafeRelease(context);
      SafeRelease(device);
      SafeRelease(source_texture);
      return acquire_status;
    }

    const guint source_subresource = gst_d3d11_memory_get_subresource_index(memory);
    context->CopySubresourceRegion(shared_texture, 0, 0, 0, 0, source_texture, source_subresource, nullptr);
    context->Flush();

    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(source_texture);
    *close_shared_handle = false;
    return SharedHandleStatus::kSuccess;
  }

  logger_t *logger = nullptr;
  uint32_t target_pid = 0;
  HANDLE target_process = nullptr;
  std::thread command_thread;
  mutable std::mutex frame_mutex;
  std::unordered_map<guint64, FrameRecord> frames;
  std::vector<SharedTexturePool> shared_texture_pools;
  std::atomic<guint64> next_frame_id{1};
  std::atomic<guint64> exported_frames{0};
  std::atomic<guint64> released_frames{0};
  std::atomic<unsigned> dropped_pool_frames{0};
  std::atomic<unsigned> reclaimed_pool_slots{0};
  std::atomic<unsigned> preempted_pool_slots{0};
  std::atomic<bool> exporting_enabled{true};
  std::atomic<bool> stopping{false};
  std::atomic<bool> stop_started{false};
  std::atomic<int> session_active_state{0};
  mutable std::atomic<bool> direct_info_logged{false};
  mutable std::atomic<bool> fallback_info_logged{false};
  std::atomic<bool> duplicate_failure_logged{false};
};

extern "C" {

shared_texture_bridge_t *shared_texture_bridge_create(logger_t *logger, uint32_t target_pid) {
  if (target_pid == 0) {
    return nullptr;
  }

  auto *bridge = new shared_texture_bridge_t(logger, target_pid);
  if (!bridge->Start()) {
    delete bridge;
    return nullptr;
  }

  return bridge;
}

void shared_texture_bridge_destroy(shared_texture_bridge_t *bridge) {
  delete bridge;
}

bool shared_texture_bridge_is_active(const shared_texture_bridge_t *bridge) {
  return bridge && bridge->IsActive();
}

void shared_texture_bridge_set_session_active(shared_texture_bridge_t *bridge, bool active,
                                              const char *reason) {
  if (!bridge) {
    return;
  }
  bridge->SetSessionActive(active, reason);
}

GstFlowReturn shared_texture_bridge_on_new_sample(shared_texture_bridge_t *bridge, GstAppSink *sink) {
  if (!bridge) {
    return GST_FLOW_OK;
  }
  return bridge->OnNewSample(sink);
}

}  // extern "C"

#else

struct shared_texture_bridge_s {};

extern "C" {

shared_texture_bridge_t *shared_texture_bridge_create(logger_t *logger, uint32_t target_pid) {
  if (logger && target_pid != 0) {
    logger_log(logger, LOGGER_WARNING,
               "shared texture export was requested for pid=%u but this build does not include Windows D3D11 bridge support",
               target_pid);
  }
  return nullptr;
}

void shared_texture_bridge_destroy(shared_texture_bridge_t *bridge) {
  (void) bridge;
}

bool shared_texture_bridge_is_active(const shared_texture_bridge_t *bridge) {
  (void) bridge;
  return false;
}

void shared_texture_bridge_set_session_active(shared_texture_bridge_t *bridge, bool active,
                                              const char *reason) {
  (void) bridge;
  (void) active;
  (void) reason;
}

GstFlowReturn shared_texture_bridge_on_new_sample(shared_texture_bridge_t *bridge, GstAppSink *sink) {
  (void) bridge;
  (void) sink;
  return GST_FLOW_OK;
}

}  // extern "C"

#endif
