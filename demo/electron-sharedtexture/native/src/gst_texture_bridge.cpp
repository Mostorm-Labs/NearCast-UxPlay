#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11memory.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kInvalidPoolIndex = std::numeric_limits<size_t>::max();
constexpr size_t kFallbackSharedTexturePoolSize = 8;

struct FrameRecord {
  guint64 id;
  GstSample *sample;
  size_t shared_texture_pool_index;
  size_t shared_texture_slot_index;
};

struct SharedTexturePoolSlot {
  ID3D11Texture2D *texture = nullptr;
  HANDLE shared_handle = nullptr;
  bool in_use = false;
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
}

void ReleaseSharedTexturePool(SharedTexturePool &pool) {
  for (auto &slot : pool.slots) {
    ReleaseSharedTexturePoolSlot(slot);
  }
  pool.slots.clear();
  SafeRelease(pool.device);
  pool.desc = {};
}

void EmitMapProbe(GstBuffer *buffer) {
  GstMapInfo map = {};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    std::cerr << "SOURCE_PROBE\tmap failed" << std::endl;
    return;
  }

  const gsize sample_length = std::min<gsize>(map.size, 256);
  guint64 sum = 0;
  guint non_zero = 0;
  for (gsize i = 0; i < sample_length; ++i) {
    const guint8 value = map.data[i];
    sum += value;
    if (value != 0) {
      ++non_zero;
    }
  }

  std::cerr << "SOURCE_PROBE\tbytes=" << sample_length << "\tnonZero=" << non_zero
            << "\tavg=" << std::fixed << std::setprecision(1)
            << (sample_length ? static_cast<double>(sum) / sample_length : 0.0) << std::endl;
  gst_buffer_unmap(buffer, &map);
}

void EmitTextureProbe(ID3D11Device *device, ID3D11Texture2D *texture, const char *label) {
  if (!device || !texture) {
    return;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);

  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.BindFlags = 0;
  staging_desc.MiscFlags = 0;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ID3D11Texture2D *staging_texture = nullptr;
  HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging_texture);
  if (FAILED(hr) || !staging_texture) {
    std::cerr << label << "\tstaging create failed\t" << FormatHRESULT(hr) << std::endl;
    return;
  }

  ID3D11DeviceContext *context = nullptr;
  device->GetImmediateContext(&context);
  if (!context) {
    SafeRelease(staging_texture);
    std::cerr << label << "\tmissing immediate context" << std::endl;
    return;
  }

  context->CopyResource(staging_texture, texture);
  context->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging_texture, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    SafeRelease(context);
    SafeRelease(staging_texture);
    std::cerr << label << "\tmap failed\t" << FormatHRESULT(hr) << std::endl;
    return;
  }

  const size_t sample_length = std::min<size_t>(256, static_cast<size_t>(desc.Height) * mapped.RowPitch);
  unsigned long long sum = 0;
  unsigned int non_zero = 0;
  const auto *bytes = static_cast<const unsigned char *>(mapped.pData);
  for (size_t i = 0; i < sample_length; ++i) {
    const unsigned char value = bytes[i];
    sum += value;
    if (value != 0) {
      ++non_zero;
    }
  }

  std::cerr << label << "\tbytes=" << sample_length << "\tnonZero=" << non_zero << "\tavg="
            << std::fixed << std::setprecision(1)
            << (sample_length ? static_cast<double>(sum) / sample_length : 0.0) << std::endl;

  context->Unmap(staging_texture, 0);
  SafeRelease(context);
  SafeRelease(staging_texture);
}

class BridgeApp {
 public:
  BridgeApp(DWORD electron_pid, std::string pipeline)
      : electron_pid_(electron_pid), pipeline_description_(std::move(pipeline)) {}

  ~BridgeApp() {
    Stop();
  }

  bool Start() {
    electron_process_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, electron_pid_);
    if (!electron_process_) {
      EmitError("failed to open Electron process for handle duplication");
      return false;
    }

    GError *error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_description_.c_str(), &error);
    if (!pipeline_) {
      EmitError(error ? error->message : "gst_parse_launch failed");
      if (error) {
        g_error_free(error);
      }
      return false;
    }

    GstElement *sink_element = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!sink_element || !GST_IS_APP_SINK(sink_element)) {
      if (sink_element) {
        gst_object_unref(sink_element);
      }
      EmitError("pipeline must contain appsink named 'sink'");
      return false;
    }

    appsink_ = GST_APP_SINK(sink_element);
    gst_app_sink_set_emit_signals(appsink_, TRUE);
    gst_app_sink_set_drop(appsink_, TRUE);
    gst_app_sink_set_max_buffers(appsink_, 2);
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(&BridgeApp::OnNewSampleThunk), this);

    bus_ = gst_element_get_bus(pipeline_);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      EmitError("failed to set pipeline to PLAYING");
      return false;
    }

    std::cout << "READY" << std::endl;
    return true;
  }

  int Run() {
    while (!stopping_) {
      PollCommands();

      GstMessage *message =
          gst_bus_timed_pop_filtered(bus_, 100 * GST_MSECOND,
                                     static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                                 GST_MESSAGE_EOS |
                                                                 GST_MESSAGE_STATE_CHANGED));
      if (!message) {
        continue;
      }

      switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
          GError *error = nullptr;
          gchar *debug = nullptr;
          gst_message_parse_error(message, &error, &debug);
          EmitError(error ? error->message : "unknown GStreamer error");
          if (debug) {
            std::cerr << "GST_DEBUG\t" << debug << std::endl;
            g_free(debug);
          }
          if (error) {
            g_error_free(error);
          }
          stopping_ = true;
          break;
        }
        case GST_MESSAGE_EOS:
          std::cout << "EOS" << std::endl;
          stopping_ = true;
          break;
        case GST_MESSAGE_STATE_CHANGED:
          if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_)) {
            GstState old_state = GST_STATE_NULL;
            GstState new_state = GST_STATE_NULL;
            GstState pending = GST_STATE_NULL;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
            std::cerr << "STATE\t" << gst_element_state_get_name(old_state) << '\t'
                      << gst_element_state_get_name(new_state) << '\t'
                      << gst_element_state_get_name(pending) << std::endl;
          }
          break;
        default:
          break;
      }

      gst_message_unref(message);
    }

    Stop();
    return 0;
  }

  void Stop() {
    bool expected = false;
    if (!stop_started_.compare_exchange_strong(expected, true)) {
      return;
    }

    stopping_ = true;

    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
    }

    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      for (auto &entry : frames_) {
        gst_sample_unref(entry.second.sample);
      }
      frames_.clear();

      for (auto &pool : shared_texture_pools_) {
        ReleaseSharedTexturePool(pool);
      }
      shared_texture_pools_.clear();
    }

    if (bus_) {
      gst_object_unref(bus_);
      bus_ = nullptr;
    }

    if (appsink_) {
      gst_object_unref(appsink_);
      appsink_ = nullptr;
    }

    if (pipeline_) {
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }

    if (electron_process_) {
      CloseHandle(electron_process_);
      electron_process_ = nullptr;
    }
  }

 private:
  static GstFlowReturn OnNewSampleThunk(GstAppSink *sink, gpointer user_data) {
    return static_cast<BridgeApp *>(user_data)->OnNewSample(sink);
  }

  GstFlowReturn OnNewSample(GstAppSink *sink) {
    if (stopping_) {
      return GST_FLOW_EOS;
    }

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
      return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
      gst_sample_unref(sample);
      EmitError("appsink sample missing buffer or caps");
      return GST_FLOW_ERROR;
    }

    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_is_d3d11_memory(memory)) {
      gst_sample_unref(sample);
      EmitError("appsink sample is not backed by GstD3D11Memory");
      return GST_FLOW_ERROR;
    }

    auto *d3d11_memory = GST_D3D11_MEMORY_CAST(memory);
    int remaining_probes = source_probe_frames_.load(std::memory_order_relaxed);
    if (remaining_probes > 0 &&
        source_probe_frames_.compare_exchange_strong(remaining_probes, remaining_probes - 1,
                                                     std::memory_order_relaxed)) {
      EmitMapProbe(buffer);
    }
    D3D11_TEXTURE2D_DESC desc = {};
    if (!gst_d3d11_memory_get_texture_desc(d3d11_memory, &desc)) {
      gst_sample_unref(sample);
      EmitError("failed to inspect D3D11 texture description");
      return GST_FLOW_ERROR;
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
        EmitError("failed to export NT handle from GstD3D11Memory or bridge-owned copy");
        return GST_FLOW_ERROR;
      }
    }

    HANDLE remote_handle = nullptr;
    BOOL duplicated =
        DuplicateHandle(GetCurrentProcess(), local_handle, electron_process_, &remote_handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    if (close_local_handle) {
      CloseHandle(local_handle);
    }

    if (!duplicated || !remote_handle) {
      gst_sample_unref(sample);
      ReleaseSharedTextureSlot(shared_texture_pool_index, shared_texture_slot_index);
      EmitError("DuplicateHandle into Electron process failed");
      return GST_FLOW_ERROR;
    }

    guint64 frame_id = next_frame_id_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      frames_.emplace(frame_id, FrameRecord{frame_id, sample, shared_texture_pool_index,
                                            shared_texture_slot_index});
    }

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
      pts = gst_util_get_timestamp();
    }

    std::ostringstream line;
    line << "FRAME\t" << frame_id << '\t' << desc.Width << '\t' << desc.Height << '\t'
         << (pts / GST_USECOND) << '\t' << "0x" << std::hex
         << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(remote_handle));
    std::cout << line.str() << std::endl;
    return GST_FLOW_OK;
  }

  void PollCommands() {
    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (!stdin_handle || stdin_handle == INVALID_HANDLE_VALUE) {
      return;
    }

    DWORD bytes_available = 0;
    if (!PeekNamedPipe(stdin_handle, nullptr, 0, nullptr, &bytes_available, nullptr)) {
      return;
    }

    if (bytes_available == 0) {
      return;
    }

    std::string line;
    while (!stopping_ && std::getline(std::cin, line)) {
      if (line == "STOP") {
        stopping_ = true;
        break;
      }

      constexpr char release_prefix[] = "RELEASE\t";
      if (line.rfind(release_prefix, 0) == 0) {
        const std::string id_text = line.substr(sizeof(release_prefix) - 1);
        char *end = nullptr;
        unsigned long long id = std::strtoull(id_text.c_str(), &end, 10);
        if (end && *end == '\0') {
          ReleaseFrame(static_cast<guint64>(id));
        }
      }

      if (!PeekNamedPipe(stdin_handle, nullptr, 0, nullptr, &bytes_available, nullptr) ||
          bytes_available == 0) {
        break;
      }
    }
  }

  void ReleaseFrame(guint64 frame_id) {
    GstSample *sample = nullptr;
    size_t shared_texture_pool_index = kInvalidPoolIndex;
    size_t shared_texture_slot_index = kInvalidPoolIndex;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      auto it = frames_.find(frame_id);
      if (it == frames_.end()) {
        return;
      }
      sample = it->second.sample;
      shared_texture_pool_index = it->second.shared_texture_pool_index;
      shared_texture_slot_index = it->second.shared_texture_slot_index;
      frames_.erase(it);
    }

    if (sample) {
      gst_sample_unref(sample);
    }
    ReleaseSharedTextureSlot(shared_texture_pool_index, shared_texture_slot_index);
  }

  void EmitError(const std::string &message) const {
    std::cerr << "ERROR\t" << message << std::endl;
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
        std::ostringstream message;
        message << "failed to create pooled shared texture: " << FormatHRESULT(hr)
                << " format=" << static_cast<unsigned>(shared_desc.Format)
                << " bind=0x" << std::hex << shared_desc.BindFlags
                << " misc=0x" << shared_desc.MiscFlags
                << " size=" << std::dec << shared_desc.Width << "x" << shared_desc.Height
                << " slot=" << slot_index;
        EmitError(message.str());
        ReleaseSharedTexturePool(created_pool);
        return false;
      }

      IDXGIResource1 *dxgi_resource = nullptr;
      hr = slot.texture->QueryInterface(IID_IDXGIResource1,
                                        reinterpret_cast<void **>(&dxgi_resource));
      if (FAILED(hr) || !dxgi_resource) {
        EmitError("failed to query IDXGIResource1 from pooled shared texture: " +
                  FormatHRESULT(hr));
        ReleaseSharedTexturePool(created_pool);
        return false;
      }

      hr = dxgi_resource->CreateSharedHandle(nullptr,
                                             DXGI_SHARED_RESOURCE_READ |
                                                 DXGI_SHARED_RESOURCE_WRITE,
                                             nullptr, &slot.shared_handle);
      SafeRelease(dxgi_resource);
      if (FAILED(hr) || !slot.shared_handle) {
        EmitError("failed to create NT handle for pooled shared texture: " +
                  FormatHRESULT(hr));
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

    std::lock_guard<std::mutex> lock(frame_mutex_);

    size_t pool_index = kInvalidPoolIndex;
    for (size_t i = 0; i < shared_texture_pools_.size(); ++i) {
      const SharedTexturePool &pool = shared_texture_pools_[i];
      if (pool.device == device && DescsMatch(pool.desc, source_desc)) {
        pool_index = i;
        break;
      }
    }

    if (pool_index == kInvalidPoolIndex) {
      shared_texture_pools_.emplace_back();
      pool_index = shared_texture_pools_.size() - 1;
      if (!InitializeSharedTexturePool(device, source_desc, &shared_texture_pools_[pool_index])) {
        shared_texture_pools_.pop_back();
        return SharedHandleStatus::kFailure;
      }
    }

    SharedTexturePool &pool = shared_texture_pools_[pool_index];
    for (size_t slot_index = 0; slot_index < pool.slots.size(); ++slot_index) {
      SharedTexturePoolSlot &slot = pool.slots[slot_index];
      if (slot.in_use) {
        continue;
      }

      slot.in_use = true;
      *pool_index_out = pool_index;
      *slot_index_out = slot_index;
      *shared_handle_out = slot.shared_handle;
      *shared_texture_out = slot.texture;
      return SharedHandleStatus::kSuccess;
    }

    const unsigned dropped = dropped_pool_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped <= 5 || dropped % 60 == 0) {
      std::ostringstream message;
      message << "dropping frame because pooled shared textures are all in use"
              << " poolSize=" << pool.slots.size()
              << " dropped=" << dropped;
      EmitError(message.str());
    }
    return SharedHandleStatus::kDropFrame;
  }

  void ReleaseSharedTextureSlot(size_t pool_index, size_t slot_index) {
    if (pool_index == kInvalidPoolIndex || slot_index == kInvalidPoolIndex) {
      return;
    }

    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (pool_index >= shared_texture_pools_.size()) {
      return;
    }

    SharedTexturePool &pool = shared_texture_pools_[pool_index];
    if (slot_index >= pool.slots.size()) {
      return;
    }

    pool.slots[slot_index].in_use = false;
  }

  SharedHandleStatus CreateShareableTextureHandle(
      GstD3D11Memory *memory, GstBuffer *buffer, GstCaps *caps,
      const D3D11_TEXTURE2D_DESC &source_desc, HANDLE *shared_handle,
      bool *close_shared_handle, size_t *shared_texture_pool_index_out,
      size_t *shared_texture_slot_index_out) {
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
      EmitError("GstD3D11Memory did not expose an ID3D11Resource");
      return SharedHandleStatus::kFailure;
    }

    ID3D11Texture2D *source_texture = nullptr;
    HRESULT hr = source_resource->QueryInterface(IID_ID3D11Texture2D,
                                                 reinterpret_cast<void **>(&source_texture));
    if (FAILED(hr) || !source_texture) {
      EmitError("failed to query ID3D11Texture2D from GstD3D11Memory: " + FormatHRESULT(hr));
      return SharedHandleStatus::kFailure;
    }

    ID3D11Device *device = nullptr;
    source_texture->GetDevice(&device);
    if (!device) {
      SafeRelease(source_texture);
      EmitError("failed to get ID3D11Device from source texture");
      return SharedHandleStatus::kFailure;
    }

    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
      SafeRelease(device);
      SafeRelease(source_texture);
      EmitError("failed to get ID3D11DeviceContext for bridge-owned shared texture");
      return SharedHandleStatus::kFailure;
    }

    GstVideoInfo video_info = {};
    if (!gst_video_info_from_caps(&video_info, caps)) {
      SafeRelease(context);
      SafeRelease(device);
      SafeRelease(source_texture);
      EmitError("failed to parse video info from caps for shared texture upload");
      return SharedHandleStatus::kFailure;
    }

    GstVideoFrame video_frame;
    if (!gst_video_frame_map(&video_frame, &video_info, buffer, GST_MAP_READ)) {
      SafeRelease(context);
      SafeRelease(device);
      SafeRelease(source_texture);
      EmitError("failed to map GstBuffer for shared texture upload");
      return SharedHandleStatus::kFailure;
    }

    ID3D11Texture2D *shared_texture = nullptr;
    const SharedHandleStatus acquire_status =
        AcquireSharedTextureSlot(device, source_desc, shared_texture_pool_index_out,
                                 shared_texture_slot_index_out, shared_handle, &shared_texture);
    if (acquire_status != SharedHandleStatus::kSuccess) {
      gst_video_frame_unmap(&video_frame);
      SafeRelease(context);
      SafeRelease(device);
      SafeRelease(source_texture);
      return acquire_status;
    }

    context->UpdateSubresource(shared_texture, 0, nullptr, GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0),
                               GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0), 0);
    gst_video_frame_unmap(&video_frame);
    context->Flush();

    int remaining_shared_probes = shared_probe_frames_.load(std::memory_order_relaxed);
    if (remaining_shared_probes > 0 &&
        shared_probe_frames_.compare_exchange_strong(remaining_shared_probes,
                                                     remaining_shared_probes - 1,
                                                     std::memory_order_relaxed)) {
      EmitTextureProbe(device, shared_texture, "SHARED_PROBE");
    }

    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(source_texture);
    *close_shared_handle = false;
    return SharedHandleStatus::kSuccess;
  }

  DWORD electron_pid_;
  std::string pipeline_description_;
  HANDLE electron_process_ = nullptr;
  GstElement *pipeline_ = nullptr;
  GstAppSink *appsink_ = nullptr;
  GstBus *bus_ = nullptr;
  std::mutex frame_mutex_;
  std::unordered_map<guint64, FrameRecord> frames_;
  std::vector<SharedTexturePool> shared_texture_pools_;
  std::atomic<guint64> next_frame_id_{1};
  std::atomic<unsigned> dropped_pool_frames_{0};
  std::atomic<int> source_probe_frames_{5};
  mutable std::atomic<int> shared_probe_frames_{5};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stop_started_{false};
};

bool ParseArguments(int argc, char **argv, DWORD &electron_pid, std::string &pipeline) {
  electron_pid = 0;
  pipeline.clear();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--electron-pid" && i + 1 < argc) {
      electron_pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
      continue;
    }

    if (arg == "--pipeline" && i + 1 < argc) {
      pipeline = argv[++i];
      continue;
    }
  }

  if (electron_pid == 0) {
    std::cerr << "ERROR\tmissing --electron-pid" << std::endl;
    return false;
  }

  if (pipeline.empty()) {
    pipeline =
        "videotestsrc is-live=true pattern=smpte ! "
        "video/x-raw,format=BGRA,width=1280,height=720,framerate=30/1 ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "d3d11upload ! "
        "video/x-raw(memory:D3D11Memory),format=BGRA ! "
        "appsink name=sink sync=false";
  }

  return true;
}

}  // namespace

int main(int argc, char **argv) {
  gst_init(&argc, &argv);

  DWORD electron_pid = 0;
  std::string pipeline;
  if (!ParseArguments(argc, argv, electron_pid, pipeline)) {
    return 1;
  }

  BridgeApp app(electron_pid, pipeline);
  if (!app.Start()) {
    return 1;
  }

  return app.Run();
}
