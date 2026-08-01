#include "printsphere/p1s_camera_client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "sdkconfig.h"

#if !defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
#error "P1sCameraClient requires CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM for its PSRAM stack"
#endif

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.camera";
constexpr uint16_t kCameraPort = 6000;
constexpr uint32_t kCameraTaskStackBytes = 12U * 1024U;
constexpr size_t kFrameHeaderBytes = 16;
constexpr size_t kMaxFrameBytes = 256U * 1024U;
constexpr size_t kImagePersistentReserveBytes = 20U * 1024U;
constexpr int64_t kTargetFrameIntervalUs =
    1000000LL / (CONFIG_PRINTSPHERE_CAMERA_TARGET_FPS > 0 ? CONFIG_PRINTSPHERE_CAMERA_TARGET_FPS
                                                           : 5);
constexpr int64_t kTlsReadDeadlineUs = 4000000;
constexpr int64_t kTlsWriteDeadlineUs = 2000000;
constexpr int kMaxFrameAttempts = 5;
constexpr uint32_t kStatsWindowFrames = 30;
constexpr size_t kPpaBufferAlignment = 128;
constexpr uint16_t kCameraViewportWidth = 1024;
constexpr uint16_t kCameraViewportHeight = 576;

struct DecodeSize {
  uint16_t width;
  uint16_t height;
};

constexpr uint16_t align_down_to_8(uint32_t value) {
  return static_cast<uint16_t>(value & ~7U);
}

constexpr DecodeSize fit_decode_size(uint16_t source_width, uint16_t source_height,
                                     uint16_t max_width, uint16_t max_height) {
  if (source_width == 0U || source_height == 0U || max_width == 0U || max_height == 0U) {
    return {0U, 0U};
  }
  if (source_width <= max_width && source_height <= max_height) {
    return {source_width, source_height};
  }

  uint32_t width = max_width;
  uint32_t height =
      (static_cast<uint32_t>(max_width) * source_height) / source_width;
  if (height > max_height) {
    height = max_height;
    width = (static_cast<uint32_t>(max_height) * source_width) / source_height;
  }
  return {align_down_to_8(width), align_down_to_8(height)};
}

static_assert(fit_decode_size(1280, 720, 1024, 576).width == 1024 &&
              fit_decode_size(1280, 720, 1024, 576).height == 576);
static_assert(fit_decode_size(1920, 1080, 1024, 576).width == 1024 &&
              fit_decode_size(1920, 1080, 1024, 576).height == 576);
static_assert(fit_decode_size(1600, 1200, 1024, 576).width == 768 &&
              fit_decode_size(1600, 1200, 1024, 576).height == 576);
static_assert(fit_decode_size(640, 360, 1024, 576).width == 640 &&
              fit_decode_size(640, 360, 1024, 576).height == 360);

bool has_frame_data(const std::shared_ptr<std::vector<uint8_t>>& frame_blob) {
  return frame_blob && !frame_blob->empty();
}

uint32_t little_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void store_le32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

std::array<uint8_t, 80> make_auth_packet(const PrinterConnection& connection) {
  std::array<uint8_t, 80> packet{};
  store_le32(packet.data(), 0x40U);
  store_le32(packet.data() + 4, 0x3000U);
  store_le32(packet.data() + 8, 0U);
  store_le32(packet.data() + 12, 0U);

  const std::string username = connection.mqtt_username.empty() ? "bblp" : connection.mqtt_username;
  const size_t username_bytes = std::min<size_t>(username.size(), 32U);
  const size_t password_bytes = std::min<size_t>(connection.access_code.size(), 32U);

  std::memcpy(packet.data() + 16, username.data(), username_bytes);
  std::memcpy(packet.data() + 48, connection.access_code.data(), password_bytes);
  return packet;
}

const char* ram_region(const void* ptr) {
  if (ptr == nullptr) {
    return "null";
  }
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  return esp_ptr_external_ram(ptr) ? "psram" : "internal";
#else
  return "internal";
#endif
}

size_t allocated_size(const void* ptr) {
  return ptr == nullptr ? 0U : heap_caps_get_allocated_size(const_cast<void*>(ptr));
}

void log_heap_diag(const char* context) {
  ESP_LOGV(kTag,
           "[RAM] %s: int_free=%u int_largest=%u dma_free=%u dma_largest=%u "
           "psram_free=%u psram_largest=%u",
           context != nullptr ? context : "-",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void log_ptr_diag(const char* context, const void* ptr, size_t bytes) {
  ESP_LOGV(kTag,
           "[RAM] %s: ptr=%p bytes=%u alloc=%u ram=%s",
           context != nullptr ? context : "-", ptr,
           static_cast<unsigned>(bytes),
           static_cast<unsigned>(allocated_size(ptr)),
           ram_region(ptr));
  log_heap_diag(context);
}

void log_blob_diag(const char* context, const std::shared_ptr<std::vector<uint8_t>>& blob) {
  const void* data = blob && !blob->empty() ? blob->data() : nullptr;
  ESP_LOGV(kTag,
           "[RAM] %s: size=%u capacity=%u alloc=%u ram=%s data=%p",
           context != nullptr ? context : "-",
           static_cast<unsigned>(blob ? blob->size() : 0U),
           static_cast<unsigned>(blob ? blob->capacity() : 0U),
           static_cast<unsigned>(allocated_size(data)),
           ram_region(data), data);
  log_heap_diag(context);
}

bool probe_jpeg_source_size(const uint8_t* data, size_t size, uint16_t* out_width,
                            uint16_t* out_height) {
  if (data == nullptr || size == 0U || out_width == nullptr || out_height == nullptr) {
    return false;
  }

  jpeg_dec_config_t probe_config = DEFAULT_JPEG_DEC_CONFIG();
  probe_config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  jpeg_dec_handle_t probe_decoder = nullptr;
  jpeg_dec_io_t probe_io = {};
  jpeg_dec_header_info_t source = {};
  if (jpeg_dec_open(&probe_config, &probe_decoder) != JPEG_ERR_OK || probe_decoder == nullptr) {
    ESP_LOGW(kTag, "JPEG probe decoder open failed");
    return false;
  }
  probe_io.inbuf = const_cast<uint8_t*>(data);
  probe_io.inbuf_len = static_cast<int>(size);
  const bool source_ok =
      jpeg_dec_parse_header(probe_decoder, &probe_io, &source) == JPEG_ERR_OK &&
      source.width > 0U && source.height > 0U;
  jpeg_dec_close(probe_decoder);
  if (!source_ok) {
    ESP_LOGW(kTag, "JPEG source header parse failed");
    return false;
  }
  *out_width = source.width;
  *out_height = source.height;
  return true;
}

// Decode a single JPEG frame at the given target size. `fallback_level` is
// echoed into the success log so callers can attribute frame rate stats.
bool decode_frame_at_target(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                            uint16_t source_width, uint16_t source_height, DecodeSize target,
                            uint8_t fallback_level,
                            std::shared_ptr<std::vector<uint8_t>>* out_blob,
                            uint16_t* out_width, uint16_t* out_height,
                            int64_t decode_started_us) {
  if (!jpeg_blob || jpeg_blob->empty() || target.width == 0U || target.height == 0U ||
      out_blob == nullptr || out_width == nullptr || out_height == nullptr) {
    return false;
  }

  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  const bool scaled = target.width != source_width || target.height != source_height;
  if (scaled) {
    config.scale.width = target.width;
    config.scale.height = target.height;
  }

  jpeg_dec_handle_t decoder = nullptr;
  jpeg_dec_io_t io = {};
  jpeg_dec_header_info_t parsed_header = {};
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) {
    ESP_LOGW(kTag, "JPEG decoder open failed for %ux%u",
             static_cast<unsigned>(target.width), static_cast<unsigned>(target.height));
    return false;
  }
  io.inbuf = const_cast<uint8_t*>(jpeg_blob->data());
  io.inbuf_len = static_cast<int>(jpeg_blob->size());

  bool decoded_ok = false;
  void* aligned_out = nullptr;
  int out_len = 0;
  do {
    if (jpeg_dec_parse_header(decoder, &io, &parsed_header) != JPEG_ERR_OK) {
      ESP_LOGW(kTag, "JPEG header parse failed for %ux%u",
               static_cast<unsigned>(target.width), static_cast<unsigned>(target.height));
      break;
    }
    if (jpeg_dec_get_outbuf_len(decoder, &out_len) != JPEG_ERR_OK || out_len <= 0) {
      ESP_LOGW(kTag, "JPEG output length query failed for %ux%u",
               static_cast<unsigned>(target.width), static_cast<unsigned>(target.height));
      break;
    }

    aligned_out = jpeg_calloc_align(static_cast<size_t>(out_len), 16);
    if (aligned_out == nullptr) {
      ESP_LOGW(kTag, "JPEG buffer allocation failed for %ux%u (%d bytes)",
               static_cast<unsigned>(target.width), static_cast<unsigned>(target.height),
               out_len);
      log_heap_diag("camera jpeg aligned allocation failed");
      break;
    }
    log_ptr_diag("camera jpeg aligned decode buffer", aligned_out,
                 static_cast<size_t>(out_len));

    io.outbuf = static_cast<uint8_t*>(aligned_out);
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
      ESP_LOGW(kTag, "JPEG decode failed for %ux%u", static_cast<unsigned>(target.width),
               static_cast<unsigned>(target.height));
      break;
    }

    auto decoded = std::make_shared<std::vector<uint8_t>>();
    decoded->reserve(std::max(static_cast<size_t>(out_len), kImagePersistentReserveBytes));
    decoded->resize(static_cast<size_t>(out_len));
    std::memcpy(decoded->data(), aligned_out, static_cast<size_t>(out_len));
    log_blob_diag("camera rgb565 decoded buffer", decoded);
    *out_blob = std::move(decoded);
    *out_width = scaled ? target.width : parsed_header.width;
    *out_height = scaled ? target.height : parsed_header.height;
    decoded_ok = true;
  } while (false);

  if (aligned_out != nullptr) {
    jpeg_free_align(aligned_out);
  }
  jpeg_dec_close(decoder);

  if (decoded_ok) {
    const uint64_t elapsed_ms =
        static_cast<uint64_t>((esp_timer_get_time() - decode_started_us) / 1000);
    ESP_LOGI(kTag,
             "Camera JPEG decoded: source=%ux%u output=%ux%u jpeg=%u rgb565=%d "
             "elapsed=%llums fallback=%u",
             static_cast<unsigned>(source_width), static_cast<unsigned>(source_height),
             static_cast<unsigned>(*out_width), static_cast<unsigned>(*out_height),
             static_cast<unsigned>(jpeg_blob->size()), out_len,
             static_cast<unsigned long long>(elapsed_ms), static_cast<unsigned>(fallback_level));
    return true;
  }
  return false;
}

// Full decode path: probe the JPEG source size, then try 1024x576 and the
// 800x448 / 400x224 fallbacks. Reports the fallback level used.
bool decode_frame_full(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                       uint16_t* out_source_width, uint16_t* out_source_height,
                       std::shared_ptr<std::vector<uint8_t>>* out_blob, uint16_t* out_width,
                       uint16_t* out_height, uint8_t* out_fallback) {
  if (!jpeg_blob || jpeg_blob->empty() || out_blob == nullptr || out_width == nullptr ||
      out_height == nullptr) {
    return false;
  }

  uint16_t source_width = 0;
  uint16_t source_height = 0;
  if (!probe_jpeg_source_size(jpeg_blob->data(), jpeg_blob->size(), &source_width,
                              &source_height)) {
    return false;
  }
  if (out_source_width != nullptr) {
    *out_source_width = source_width;
  }
  if (out_source_height != nullptr) {
    *out_source_height = source_height;
  }

  const std::array<DecodeSize, 3> candidates = {
      fit_decode_size(source_width, source_height, kCameraViewportWidth, kCameraViewportHeight),
      fit_decode_size(source_width, source_height, 800, 448),
      fit_decode_size(source_width, source_height, 400, 224),
  };
  const int64_t decode_started_us = esp_timer_get_time();
  DecodeSize previous = {0U, 0U};

  for (size_t attempt = 0; attempt < candidates.size(); ++attempt) {
    const DecodeSize target = candidates[attempt];
    if (target.width == 0U || target.height == 0U ||
        (target.width == previous.width && target.height == previous.height)) {
      continue;
    }
    previous = target;
    if (decode_frame_at_target(jpeg_blob, source_width, source_height, target,
                               static_cast<uint8_t>(attempt), out_blob, out_width, out_height,
                               decode_started_us)) {
      if (out_fallback != nullptr) {
        *out_fallback = static_cast<uint8_t>(attempt);
      }
      return true;
    }
  }

  ESP_LOGW(kTag, "JPEG decode failed at all quality levels (source=%ux%u)",
           static_cast<unsigned>(source_width), static_cast<unsigned>(source_height));
  return false;
}

}  // namespace

void P1sCameraClient::configure(PrinterConnection connection) {
  if (connection.mqtt_username.empty()) {
    connection.mqtt_username = "bblp";
  }
  const bool configured = connection.is_ready();

  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    desired_connection_ = std::move(connection);
  }
  reconfigure_requested_.store(true);

  P1sCameraSnapshot snapshot;
  snapshot.configured = configured;
  snapshot.enabled = false;
  snapshot.connected = false;
  snapshot.detail = snapshot.configured ? "Camera snapshot ready" : "Camera not configured";
  set_snapshot(std::move(snapshot));
}

bool P1sCameraClient::is_configured() const {
  return desired_connection().is_ready();
}

PrinterConnection P1sCameraClient::desired_connection() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return desired_connection_;
}

esp_err_t P1sCameraClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }

  // Place task stack in PSRAM. By the time start() is called, internal RAM is
  // fragmented (Wi-Fi + Cloud MQTT TLS + 44 KB rotation staging) and a 12 KiB
  // contiguous internal-RAM stack allocation reliably fails.
  const BaseType_t result = xTaskCreateWithCaps(
      &P1sCameraClient::task_entry, "p1s_camera", kCameraTaskStackBytes, this, 4, &task_handle_,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (result != pdPASS) {
    return ESP_FAIL;
  }

  StackType_t* task_stack = nullptr;
  StaticTask_t* task_tcb = nullptr;
  const BaseType_t buffers_result =
      xTaskGetStaticBuffers(task_handle_, &task_stack, &task_tcb);
  ESP_LOGI(kTag, "Camera task started: stack=%u bytes ram=%s",
           static_cast<unsigned>(kCameraTaskStackBytes),
           buffers_result == pdTRUE ? ram_region(task_stack) : "unknown");
  return ESP_OK;
}

P1sCameraSnapshot P1sCameraClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void P1sCameraClient::observe_printer_snapshot(const PrinterSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  observed_model_ = snapshot.local_model;
  observed_rtsp_url_ = snapshot.camera_rtsp_url;
  observed_signature_required_ = snapshot.local_mqtt_signature_required;
}

PrinterModel P1sCameraClient::observed_model() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_model_;
}

std::string P1sCameraClient::observed_rtsp_url() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_rtsp_url_;
}

bool P1sCameraClient::observed_signature_required() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_signature_required_;
}

void P1sCameraClient::task_entry(void* context) {
  static_cast<P1sCameraClient*>(context)->task_loop();
}

void P1sCameraClient::set_snapshot(P1sCameraSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
}

void P1sCameraClient::set_status_snapshot(bool configured, bool enabled, bool connected,
                                          const char* detail) {
  const std::string next_detail = detail != nullptr ? detail : "";
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.configured == configured && snapshot_.enabled == enabled &&
      snapshot_.connected == connected && snapshot_.detail == next_detail) {
    return;
  }
  snapshot_.configured = configured;
  snapshot_.enabled = enabled;
  snapshot_.connected = connected;
  snapshot_.detail = next_detail;
}

void P1sCameraClient::set_frame_snapshot(bool configured, bool enabled, bool connected,
                                         const char* detail,
                                         std::shared_ptr<std::vector<uint8_t>> frame_blob,
                                         uint16_t width, uint16_t height) {
  const std::string next_detail = detail != nullptr ? detail : "";
  const void* next_blob = frame_blob.get();
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.configured == configured && snapshot_.enabled == enabled &&
      snapshot_.connected == connected && snapshot_.detail == next_detail &&
      snapshot_.frame_blob.get() == next_blob && snapshot_.width == width &&
      snapshot_.height == height) {
    return;
  }
  snapshot_.configured = configured;
  snapshot_.enabled = enabled;
  snapshot_.connected = connected;
  snapshot_.detail = next_detail;
  snapshot_.frame_blob = std::move(frame_blob);
  snapshot_.width = width;
  snapshot_.height = height;
}

bool P1sCameraClient::has_cached_frame() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_frame_data(snapshot_.frame_blob);
}

bool P1sCameraClient::read_exact(esp_tls_t* tls, void* buffer, size_t length) {
  auto* out = static_cast<uint8_t*>(buffer);
  size_t offset = 0;
  const int64_t deadline_us = esp_timer_get_time() + kTlsReadDeadlineUs;
  while (offset < length) {
    if (esp_timer_get_time() >= deadline_us) {
      return false;
    }
    const ssize_t read = esp_tls_conn_read(tls, out + offset, length - offset);
    if (read <= 0) {
      return false;
    }
    offset += static_cast<size_t>(read);
  }
  return true;
}

bool P1sCameraClient::write_all(esp_tls_t* tls, const void* buffer, size_t length) {
  const auto* data = static_cast<const uint8_t*>(buffer);
  size_t offset = 0;
  const int64_t deadline_us = esp_timer_get_time() + kTlsWriteDeadlineUs;
  while (offset < length) {
    if (esp_timer_get_time() >= deadline_us) {
      return false;
    }
    const ssize_t written = esp_tls_conn_write(tls, data + offset, length - offset);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool P1sCameraClient::ensure_connected(const PrinterConnection& connection) {
  if (tls_ != nullptr) {
    return true;
  }

  log_heap_diag("camera before tls init");

  esp_tls_cfg_t tls_cfg = {};
  tls_cfg.timeout_ms = 7000;
  tls_cfg.skip_common_name = true;
  tls_cfg.addr_family = ESP_TLS_AF_INET;
  tls_cfg.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  tls_cfg.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif

  tls_ = esp_tls_init();
  if (tls_ == nullptr) {
    ESP_LOGW(kTag, "Failed to allocate TLS context");
    return false;
  }

  const int connect_result = esp_tls_conn_new_sync(connection.host.c_str(),
                                                   static_cast<int>(connection.host.size()),
                                                   kCameraPort, &tls_cfg, tls_);
  if (connect_result != 1) {
    ESP_LOGW(kTag, "Camera TLS connect failed");
    disconnect();
    return false;
  }

  const auto auth_packet = make_auth_packet(connection);
  if (!write_all(tls_, auth_packet.data(), auth_packet.size())) {
    ESP_LOGW(kTag, "Camera auth write failed");
    disconnect();
    return false;
  }

  ESP_LOGI(kTag, "Connected to local chamber camera on %s:%u", connection.host.c_str(),
           static_cast<unsigned>(kCameraPort));
  log_heap_diag("camera after tls auth");
  return true;
}

void P1sCameraClient::disconnect() {
  if (tls_ != nullptr) {
    esp_tls_conn_destroy(tls_);
    tls_ = nullptr;
  }
}

bool P1sCameraClient::decode_frame_rgb565(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                          std::shared_ptr<std::vector<uint8_t>>* out_blob,
                                          uint16_t* out_width, uint16_t* out_height) {
  return decode_frame_full(jpeg_blob, nullptr, nullptr, out_blob, out_width, out_height,
                           nullptr);
}

bool P1sCameraClient::decode_frame_rgb565_cached(
    const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
    std::shared_ptr<std::vector<uint8_t>>* out_blob, uint16_t* out_width,
    uint16_t* out_height) {
  if (!jpeg_blob || jpeg_blob->empty() || out_blob == nullptr || out_width == nullptr ||
      out_height == nullptr) {
    return false;
  }

  // Fast path: reuse the last successful source + decode size with the
  // hardware engine, skipping the header probe pass. The P1 camera source
  // resolution is stable in practice; any failure clears the cache and falls
  // back to a fresh probe, then to the software chain.
  if (cached_source_width_ != 0U && cached_source_height_ != 0U &&
      cached_decode_width_ != 0U && cached_decode_height_ != 0U) {
    if (decode_frame_hw(jpeg_blob, cached_source_width_, cached_source_height_,
                        cached_decode_width_, cached_decode_height_, out_blob, out_width,
                        out_height)) {
      last_decode_fallback_ = 0;
      return true;
    }
    cached_source_width_ = 0U;
    cached_source_height_ = 0U;
    cached_decode_width_ = 0U;
    cached_decode_height_ = 0U;
  }

  // Hardware path with an explicit header probe for the source size.
  {
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpeg_blob->data(), static_cast<uint32_t>(jpeg_blob->size()),
                              &info) == ESP_OK &&
        info.width > 0U && info.height > 0U) {
      const uint16_t source_width = static_cast<uint16_t>(info.width);
      const uint16_t source_height = static_cast<uint16_t>(info.height);
      const DecodeSize target =
          fit_decode_size(source_width, source_height, kCameraViewportWidth,
                          kCameraViewportHeight);
      if (target.width > 0U && target.height > 0U &&
          decode_frame_hw(jpeg_blob, source_width, source_height, target.width, target.height,
                          out_blob, out_width, out_height)) {
        cached_source_width_ = source_width;
        cached_source_height_ = source_height;
        cached_decode_width_ = *out_width;
        cached_decode_height_ = *out_height;
        last_decode_fallback_ = 0;
        return true;
      }
    }
  }

  // Software fallback chain (probe + 1024x576 / 800x448 / 400x224).
  uint16_t source_width = 0;
  uint16_t source_height = 0;
  uint8_t fallback = 0;
  if (!decode_frame_full(jpeg_blob, &source_width, &source_height, out_blob, out_width,
                         out_height, &fallback)) {
    last_decode_fallback_ = 0;
    return false;
  }
  cached_source_width_ = source_width;
  cached_source_height_ = source_height;
  cached_decode_width_ = *out_width;
  cached_decode_height_ = *out_height;
  last_decode_fallback_ = fallback;
  return true;
}

bool P1sCameraClient::ensure_hw_resources(uint16_t source_width, uint16_t source_height,
                                          uint16_t target_width, uint16_t target_height) {
  if (hw_init_attempted_) {
    return hw_resources_ok_;
  }
  hw_init_attempted_ = true;

  if (hw_decoder_ == nullptr) {
    jpeg_decode_engine_cfg_t engine_cfg = {};
    engine_cfg.timeout_ms = 100;
    if (jpeg_new_decoder_engine(&engine_cfg, &hw_decoder_) != ESP_OK ||
        hw_decoder_ == nullptr) {
      ESP_LOGW(kTag, "HW JPEG engine open failed");
      hw_decoder_ = nullptr;
      return false;
    }
    ESP_LOGI(kTag, "HW JPEG decoder engine ready");
  }

  if (ppa_client_ == nullptr) {
    ppa_client_config_t client_cfg = {};
    client_cfg.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&client_cfg, &ppa_client_) != ESP_OK || ppa_client_ == nullptr) {
      ESP_LOGW(kTag, "PPA SRM client register failed");
      ppa_client_ = nullptr;
      return false;
    }
    ESP_LOGI(kTag, "PPA SRM client ready");
  }

  const size_t decode_bytes = static_cast<size_t>(source_width) * source_height * sizeof(uint16_t);
  const size_t scaled_bytes = static_cast<size_t>(target_width) * target_height * sizeof(uint16_t);
  if (hw_decode_buf_ == nullptr || hw_decode_buf_cap_ < decode_bytes) {
    if (hw_decode_buf_ != nullptr) {
      heap_caps_free(hw_decode_buf_);
      hw_decode_buf_ = nullptr;
    }
    hw_decode_buf_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kPpaBufferAlignment, decode_bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    hw_decode_buf_cap_ = hw_decode_buf_ != nullptr ? decode_bytes : 0U;
  }
  if (ppa_out_buf_ == nullptr || ppa_out_buf_cap_ < scaled_bytes) {
    if (ppa_out_buf_ != nullptr) {
      heap_caps_free(ppa_out_buf_);
      ppa_out_buf_ = nullptr;
    }
    ppa_out_buf_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kPpaBufferAlignment, scaled_bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    ppa_out_buf_cap_ = ppa_out_buf_ != nullptr ? scaled_bytes : 0U;
  }

  if (hw_decode_buf_ == nullptr || ppa_out_buf_ == nullptr) {
    ESP_LOGW(kTag, "HW camera buffers allocation failed: decode=%u scaled=%u",
             static_cast<unsigned>(decode_bytes), static_cast<unsigned>(scaled_bytes));
    log_heap_diag("camera hw buffer allocation failed");
    return false;
  }

  hw_resources_ok_ = true;
  return true;
}

bool P1sCameraClient::decode_frame_hw(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                      uint16_t source_width, uint16_t source_height,
                                      uint16_t target_width, uint16_t target_height,
                                      std::shared_ptr<std::vector<uint8_t>>* out_blob,
                                      uint16_t* out_width, uint16_t* out_height) {
  if (!jpeg_blob || jpeg_blob->empty() || out_blob == nullptr || out_width == nullptr ||
      out_height == nullptr || source_width == 0U || source_height == 0U ||
      target_width == 0U || target_height == 0U) {
    return false;
  }
  if (!ensure_hw_resources(source_width, source_height, target_width, target_height)) {
    return false;
  }

  jpeg_decode_cfg_t decode_cfg = {};
  decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  // Keep the default BGR element order: the DMA2D scramble for RGB order
  // emits big-endian RGB565, which neither PPA nor LVGL (little-endian
  // RGB565) can consume directly.
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

  const int64_t decode_started_us = esp_timer_get_time();
  uint32_t decoded_size = 0;
  const esp_err_t dec_err = jpeg_decoder_process(
      hw_decoder_, &decode_cfg, jpeg_blob->data(), static_cast<uint32_t>(jpeg_blob->size()),
      hw_decode_buf_, static_cast<uint32_t>(hw_decode_buf_cap_), &decoded_size);
  if (dec_err != ESP_OK) {
    ESP_LOGW(kTag, "HW JPEG decode failed: %s", esp_err_to_name(dec_err));
    return false;
  }
  ESP_LOGI(kTag, "HW JPEG decoded_size=%u expected=%u", static_cast<unsigned>(decoded_size),
           static_cast<unsigned>(static_cast<size_t>(source_width) * source_height * 2U));

  uint16_t out_w = source_width;
  uint16_t out_h = source_height;
  const uint8_t* pixel_src = hw_decode_buf_;
  // TEMP: bypass PPA scaling to isolate edge-garbage source (JPEG vs PPA).
  if (false && (source_width > target_width || source_height > target_height)) {
    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = hw_decode_buf_;
    srm.in.pic_w = source_width;
    srm.in.pic_h = source_height;
    srm.in.block_w = source_width;
    srm.in.block_h = source_height;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.out.buffer = ppa_out_buf_;
    srm.out.buffer_size = static_cast<uint32_t>(ppa_out_buf_cap_);
    srm.out.pic_w = target_width;
    srm.out.pic_h = target_height;
    srm.out.block_offset_x = 0;
    srm.out.block_offset_y = 0;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm.scale_x = static_cast<float>(target_width) / static_cast<float>(source_width);
    srm.scale_y = static_cast<float>(target_height) / static_cast<float>(source_height);
    srm.mirror_x = false;
    srm.mirror_y = false;
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t ppa_err = ppa_do_scale_rotate_mirror(ppa_client_, &srm);
    if (ppa_err != ESP_OK) {
      ESP_LOGW(kTag, "PPA scale-rotate failed: %s", esp_err_to_name(ppa_err));
      return false;
    }
    out_w = target_width;
    out_h = target_height;
    pixel_src = ppa_out_buf_;
  }

  const size_t pixel_bytes = static_cast<size_t>(out_w) * out_h * sizeof(uint16_t);
  const size_t pixel_capacity =
      pixel_src == hw_decode_buf_ ? hw_decode_buf_cap_ : ppa_out_buf_cap_;
  if (pixel_bytes > pixel_capacity) {
    ESP_LOGW(kTag, "HW decode output size unexpected: %ux%u", static_cast<unsigned>(out_w),
             static_cast<unsigned>(out_h));
    return false;
  }

  auto decoded = std::make_shared<std::vector<uint8_t>>();
  decoded->reserve(std::max(pixel_bytes, kImagePersistentReserveBytes));
  decoded->resize(pixel_bytes);
  std::memcpy(decoded->data(), pixel_src, pixel_bytes);
  *out_blob = std::move(decoded);
  *out_width = out_w;
  *out_height = out_h;

  const uint64_t elapsed_ms =
      static_cast<uint64_t>((esp_timer_get_time() - decode_started_us) / 1000);
  ESP_LOGI(kTag,
           "Camera JPEG HW decoded: source=%ux%u output=%ux%u jpeg=%u rgb565=%u "
           "elapsed=%llums",
           static_cast<unsigned>(source_width), static_cast<unsigned>(source_height),
           static_cast<unsigned>(out_w), static_cast<unsigned>(out_h),
           static_cast<unsigned>(jpeg_blob->size()), static_cast<unsigned>(pixel_bytes),
           static_cast<unsigned long long>(elapsed_ms));
  return true;
}

bool P1sCameraClient::fetch_next_frame(const PrinterConnection& connection) {
  if (!connection.is_ready()) {
    return false;
  }

  if (!ensure_connected(connection)) {
    return false;
  }

  for (int frame_index = 0; frame_index < kMaxFrameAttempts; ++frame_index) {
    if (frame_index > 0 && !enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled mid-fetch, aborting");
      return false;
    }
    uint8_t header[kFrameHeaderBytes] = {};
    if (!read_exact(tls_, header, sizeof(header))) {
      ESP_LOGW(kTag, "Camera frame header read failed");
      disconnect();
      return false;
    }

    const uint32_t reserved0 = little_u32(header + 4);
    const uint32_t frame_marker = little_u32(header + 8);
    const uint32_t reserved1 = little_u32(header + 12);
    if (reserved0 != 0U || reserved1 != 0U || (frame_marker != 0U && frame_marker != 1U)) {
      ESP_LOGW(kTag, "Camera frame header unexpected: size=%u marker=%u r0=%u r1=%u",
               static_cast<unsigned>(little_u32(header)), static_cast<unsigned>(frame_marker),
               static_cast<unsigned>(reserved0), static_cast<unsigned>(reserved1));
      disconnect();
      return false;
    }

    const uint32_t frame_size = little_u32(header);
    if (frame_size < 4U || frame_size > kMaxFrameBytes) {
      ESP_LOGW(kTag, "Camera frame size invalid: %u", static_cast<unsigned>(frame_size));
      disconnect();
      return false;
    }

    auto jpeg_frame = std::make_shared<std::vector<uint8_t>>();
    jpeg_frame->reserve(std::max(static_cast<size_t>(frame_size), kImagePersistentReserveBytes));
    jpeg_frame->resize(frame_size);
    if (!read_exact(tls_, jpeg_frame->data(), jpeg_frame->size())) {
      ESP_LOGW(kTag, "Camera frame body read failed");
      disconnect();
      return false;
    }
    log_blob_diag("camera jpeg frame buffer", jpeg_frame);

    if (!enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled before decode, dropping received frame");
      return false;
    }

    if ((*jpeg_frame)[0] != 0xFFU || (*jpeg_frame)[1] != 0xD8U ||
        (*jpeg_frame)[jpeg_frame->size() - 2] != 0xFFU ||
        (*jpeg_frame)[jpeg_frame->size() - 1] != 0xD9U) {
      ESP_LOGW(kTag, "Camera frame is not a complete JPEG");
      continue;
    }

    std::shared_ptr<std::vector<uint8_t>> rgb565_frame;
    uint16_t width = 0;
    uint16_t height = 0;
    if (!decode_frame_rgb565_cached(jpeg_frame, &rgb565_frame, &width, &height)) {
      continue;
    }

    if (!enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled after decode, dropping frame");
      return false;
    }

    set_frame_snapshot(true, enabled_.load(), true, "Camera image updated",
                       std::move(rgb565_frame), width, height);
    ESP_LOGI(kTag, "Camera snapshot decoded: %ux%u RGB565", static_cast<unsigned>(width),
             static_cast<unsigned>(height));
    log_heap_diag("camera after snapshot publish");
    return true;
  }

  ESP_LOGW(kTag, "No valid camera JPEG frame received");
  return false;
}

void P1sCameraClient::task_loop() {
  while (true) {
    if (reconfigure_requested_.exchange(false)) {
      disconnect();
      refresh_requested_.store(false);
      idle_notified_ = false;
      consecutive_connect_failures_ = 0;
      stats_window_frames_ = 0;
      stats_window_fallbacks_ = 0;
      stats_window_fetch_total_us_ = 0;
      stats_window_start_us_ = 0;
    }

    const PrinterConnection connection = desired_connection();
    if (!connection.is_ready()) {
      idle_notified_ = false;
      set_status_snapshot(false, false, false, "Camera not configured");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!network_ready_.load()) {
      disconnect();
      idle_notified_ = false;
      set_status_snapshot(true, enabled_.load(), false, "Camera waiting for Wi-Fi");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!enabled_.load()) {
      disconnect();
      if (!idle_notified_) {
        set_status_snapshot(true, false, false, has_cached_frame() ? "Tap for new image"
                                                                   : "Camera off");
        idle_notified_ = true;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    idle_notified_ = false;

    const PrinterModel model = observed_model();
    const std::string rtsp_url = observed_rtsp_url();
    const bool signature_required = observed_signature_required();
    const bool rtsp_camera = printer_model_has_rtsp_camera(model) || !rtsp_url.empty();
    const bool jpeg_camera =
        !rtsp_camera && (model == PrinterModel::kUnknown || printer_model_has_jpeg_camera(model));
    if (rtsp_camera) {
      disconnect();

      std::string detail;
      if (signature_required) {
        detail = "Enable Developer Mode for RTSP live view";
      } else if (rtsp_url.empty()) {
        detail = "RTSP live view disabled on printer";
      } else {
        detail = "RTSP live view not supported yet on this model";
      }

      set_frame_snapshot(true, true, false, detail.c_str(), nullptr, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!jpeg_camera) {
      disconnect();
      set_frame_snapshot(true, true, false, "Live camera not available on this model", nullptr, 0,
                         0);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Continuous frame loop: keep the TLS connection and read at the target
    // rate. Manual refresh requests skip the pacing wait. Any failure exits
    // to the outer loop so gating (reconfigure / Wi-Fi / enabled) is
    // re-evaluated.
    while (enabled_.load() && network_ready_.load() && !reconfigure_requested_.load()) {
      const bool refresh_forced = refresh_requested_.exchange(false);

      set_status_snapshot(true, true, false, "Loading camera image");
      const int64_t fetch_started_us = esp_timer_get_time();
      const bool ok = fetch_next_frame(connection);
      const int64_t fetch_elapsed_us = esp_timer_get_time() - fetch_started_us;
      if (!ok) {
        if (!enabled_.load()) {
          consecutive_connect_failures_ = 0;
          break;
        }
        ++consecutive_connect_failures_;
        const uint32_t backoff_ms =
            consecutive_connect_failures_ <= 1 ? 2000U :
            consecutive_connect_failures_ <= 2 ? 4000U :
            consecutive_connect_failures_ <= 4 ? 8000U :
            consecutive_connect_failures_ <= 6 ? 15000U : 30000U;
        set_status_snapshot(true, enabled_.load(), false, "Camera image failed");
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        break;
      }
      consecutive_connect_failures_ = 0;

      ++stats_window_frames_;
      stats_window_fallbacks_ += last_decode_fallback_ > 0U ? 1U : 0U;
      stats_window_fetch_total_us_ += fetch_elapsed_us;
      if (stats_window_start_us_ == 0) {
        stats_window_start_us_ = esp_timer_get_time();
      }
      if (stats_window_frames_ >= kStatsWindowFrames) {
        const int64_t window_us = esp_timer_get_time() - stats_window_start_us_;
        const float fps =
            window_us > 0 ? (kStatsWindowFrames * 1000000.0f) / static_cast<float>(window_us)
                          : 0.0f;
        const float avg_fetch_ms =
            stats_window_frames_ > 0
                ? static_cast<float>(stats_window_fetch_total_us_) /
                      (stats_window_frames_ * 1000.0f)
                : 0.0f;
        ESP_LOGI(kTag, "Camera stats: fps=%.1f avg_fetch=%.1fms frames=%u fallbacks=%u",
                 static_cast<double>(fps), static_cast<double>(avg_fetch_ms),
                 static_cast<unsigned>(stats_window_frames_),
                 static_cast<unsigned>(stats_window_fallbacks_));
        stats_window_frames_ = 0;
        stats_window_fallbacks_ = 0;
        stats_window_fetch_total_us_ = 0;
        stats_window_start_us_ = 0;
      }

      // Adaptive pacing: sleep the remainder of the target interval after
      // accounting for this frame's fetch+decode time. A slow network or
      // decode naturally lowers the achieved frame rate.
      if (!refresh_forced) {
        const int64_t wait_us = kTargetFrameIntervalUs - fetch_elapsed_us;
        if (wait_us > 0) {
          vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        }
      }
    }
  }
}

}  // namespace printsphere
