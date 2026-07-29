#include "printsphere/pmu.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.power";
constexpr adc_unit_t kAdcUnit = ADC_UNIT_2;
constexpr adc_channel_t kAdcChannel = ADC_CHANNEL_3;
constexpr adc_atten_t kAdcAttenuation = ADC_ATTEN_DB_12;
constexpr int kSampleCount = 16;
constexpr int kEmptyMillivolts = 3300;
constexpr int kFullMillivolts = 4200;
constexpr int kDividerTopOhms = 68000;
constexpr int kDividerBottomOhms = 100000;

adc_oneshot_unit_handle_t s_adc;
adc_cali_handle_t s_calibration;
bool s_calibrated;

uint8_t battery_percent(int millivolts) {
  if (millivolts <= kEmptyMillivolts) {
    return 0;
  }
  if (millivolts >= kFullMillivolts) {
    return 100;
  }
  return static_cast<uint8_t>((millivolts - kEmptyMillivolts) * 100 /
                              (kFullMillivolts - kEmptyMillivolts));
}
}  // namespace

esp_err_t PmuManager::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = kAdcUnit;
  ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), kTag,
                      "create ADC2 unit failed");

  adc_oneshot_chan_cfg_t channel_cfg = {};
  channel_cfg.atten = kAdcAttenuation;
  channel_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, kAdcChannel, &channel_cfg), kTag,
                      "configure battery ADC failed");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t calibration_cfg = {};
  calibration_cfg.unit_id = kAdcUnit;
  calibration_cfg.chan = kAdcChannel;
  calibration_cfg.atten = kAdcAttenuation;
  calibration_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  s_calibrated = adc_cali_create_scheme_curve_fitting(&calibration_cfg, &s_calibration) == ESP_OK;
#endif
  initialized_ = true;
  ESP_LOGI(kTag, "Battery ADC ready on GPIO52 (calibrated=%d)", s_calibrated ? 1 : 0);
  return ESP_OK;
}

PowerSnapshot PmuManager::sample() const {
  PowerSnapshot snapshot;
  if (!initialized_ || s_adc == nullptr) {
    return snapshot;
  }

  int raw_sum = 0;
  int voltage_sum = 0;
  for (int i = 0; i < kSampleCount; ++i) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, kAdcChannel, &raw) != ESP_OK) {
      return snapshot;
    }
    raw_sum += raw;
    int voltage_mv = raw * 3300 / 4095;
    if (s_calibrated && adc_cali_raw_to_voltage(s_calibration, raw, &voltage_mv) != ESP_OK) {
      return snapshot;
    }
    voltage_sum += voltage_mv;
  }

  (void)raw_sum;
  const int adc_mv = voltage_sum / kSampleCount;
  const int battery_mv = adc_mv * (kDividerTopOhms + kDividerBottomOhms) /
                         kDividerBottomOhms;
  snapshot.available = true;
  snapshot.battery_present = battery_mv >= 3000 && battery_mv <= 4300;
  snapshot.battery_percent = snapshot.battery_present ? battery_percent(battery_mv) : 0;
  // This board exposes the battery divider but no validated VBUS/charger-status
  // signal. Do not infer external power from battery presence.
  snapshot.external_power_known = false;
  snapshot.charging = false;
  snapshot.usb_present = false;
  snapshot.temperature_c = 0.0f;
  return snapshot;
}

}  // namespace printsphere
