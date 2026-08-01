#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_jpeg_dec.h"
#include "esp_tls.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

namespace printsphere {

struct P1sCameraSnapshot {
  bool configured = false;
  bool enabled = false;
  bool connected = false;
  std::string detail = "Live camera off";
  std::shared_ptr<std::vector<uint8_t>> frame_blob;
  uint16_t width = 0;
  uint16_t height = 0;
};

class P1sCameraClient {
 public:
  P1sCameraClient() = default;

  void configure(PrinterConnection connection);
  bool is_configured() const;
  void set_network_ready(bool ready) { network_ready_.store(ready); }
  void set_enabled(bool enabled) { enabled_.store(enabled); }
  void request_refresh() { refresh_requested_.store(true); }
  void observe_printer_snapshot(const PrinterSnapshot& snapshot);
  esp_err_t start();
  P1sCameraSnapshot snapshot() const;

 private:
  static void task_entry(void* context);

  void task_loop();
  PrinterConnection desired_connection() const;
  bool ensure_connected(const PrinterConnection& connection);
  void disconnect();
  bool fetch_next_frame(const PrinterConnection& connection);
  bool decode_frame_rgb565_cached(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                  std::shared_ptr<std::vector<uint8_t>>* out_blob,
                                  uint16_t* out_width, uint16_t* out_height);
  void set_snapshot(P1sCameraSnapshot snapshot);
  void set_status_snapshot(bool configured, bool enabled, bool connected, const char* detail);
  void set_frame_snapshot(bool configured, bool enabled, bool connected, const char* detail,
                          std::shared_ptr<std::vector<uint8_t>> frame_blob, uint16_t width,
                          uint16_t height);
  PrinterModel observed_model() const;
  std::string observed_rtsp_url() const;
  bool observed_signature_required() const;
  bool has_cached_frame() const;
  static bool read_exact(esp_tls_t* tls, void* buffer, size_t length);
  static bool write_all(esp_tls_t* tls, const void* buffer, size_t length);
  static bool decode_frame_rgb565(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                  std::shared_ptr<std::vector<uint8_t>>* out_blob, uint16_t* out_width,
                                  uint16_t* out_height);
  bool ensure_hw_resources(uint16_t source_width, uint16_t source_height, uint16_t target_width,
                           uint16_t target_height);
  bool decode_frame_hw(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                       uint16_t source_width, uint16_t source_height, uint16_t target_width,
                       uint16_t target_height, std::shared_ptr<std::vector<uint8_t>>* out_blob,
                       uint16_t* out_width, uint16_t* out_height);

  mutable std::mutex mutex_{};
  P1sCameraSnapshot snapshot_{};
  mutable std::mutex config_mutex_{};
  PrinterConnection desired_connection_{};
  mutable std::mutex observed_mutex_{};
  PrinterModel observed_model_ = PrinterModel::kUnknown;
  std::string observed_rtsp_url_{};
  bool observed_signature_required_ = false;
  TaskHandle_t task_handle_ = nullptr;
  esp_tls_t* tls_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> refresh_requested_{false};
  std::atomic<bool> reconfigure_requested_{false};
  bool idle_notified_ = false;
  uint32_t consecutive_connect_failures_ = 0;
  bool hw_init_attempted_ = false;
  bool hw_resources_ok_ = false;
  // Decode-size cache and frame-rate statistics (camera task thread only).
  uint16_t cached_source_width_ = 0;
  uint16_t cached_source_height_ = 0;
  uint16_t cached_decode_width_ = 0;
  uint16_t cached_decode_height_ = 0;
  uint8_t last_decode_fallback_ = 0;
  uint32_t stats_window_frames_ = 0;
  uint32_t stats_window_fallbacks_ = 0;
  int64_t stats_window_fetch_total_us_ = 0;
  int64_t stats_window_start_us_ = 0;
  // Hardware decode + PPA scale resources (camera task thread only).
  jpeg_decoder_handle_t hw_decoder_ = nullptr;
  ppa_client_handle_t ppa_client_ = nullptr;
  uint8_t* hw_decode_buf_ = nullptr;
  size_t hw_decode_buf_cap_ = 0;
  uint8_t* ppa_out_buf_ = nullptr;
  size_t ppa_out_buf_cap_ = 0;
};

}  // namespace printsphere
