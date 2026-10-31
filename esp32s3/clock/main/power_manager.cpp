#include "power_manager.h"

#include <cstdlib>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "portmacro.h"
#include "sdkconfig.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static const char *TAG = "PowerManager";
static constexpr uint8_t AXP2101_ADDRESS = 0x34;
static constexpr int AXP2101_I2C_TIMEOUT_MS = 1000;

static XPowersPMU s_pmu;
static i2c_master_dev_handle_t s_pmu_device = nullptr;
static bool s_pmu_ready = false;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_status_t s_status = {};

static int pmu_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    (void)dev_addr;
    if (s_pmu_device == nullptr || data == nullptr || len == 0) {
        return -1;
    }
    const esp_err_t err = i2c_master_transmit_receive(s_pmu_device,
                                                      &reg_addr,
                                                      sizeof(reg_addr),
                                                      data,
                                                      len,
                                                      AXP2101_I2C_TIMEOUT_MS);
    return err == ESP_OK ? 0 : -1;
}

static int pmu_write(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t len)
{
    (void)dev_addr;
    if (s_pmu_device == nullptr || data == nullptr || len == 0) {
        return -1;
    }

    uint8_t *buffer = static_cast<uint8_t *>(std::malloc(len + 1));
    if (buffer == nullptr) {
        return -1;
    }
    buffer[0] = reg_addr;
    std::memcpy(&buffer[1], data, len);
    const esp_err_t err = i2c_master_transmit(s_pmu_device,
                                              buffer,
                                              len + 1,
                                              AXP2101_I2C_TIMEOUT_MS);
    std::free(buffer);
    return err == ESP_OK ? 0 : -1;
}

esp_err_t power_manager_init(void)
{
    if (s_pmu_ready) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        ESP_LOGW(TAG, "BSP I2C bus is unavailable");
        return ESP_FAIL;
    }

    if (s_pmu_device == nullptr) {
        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = AXP2101_ADDRESS;
        device_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
        const esp_err_t err = i2c_master_bus_add_device(bus, &device_config, &s_pmu_device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Add AXP2101 device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_pmu.begin(AXP2101_SLAVE_ADDRESS, pmu_read, pmu_write)) {
        ESP_LOGW(TAG, "AXP2101 init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AXP2101 detected, chip id=0x%02x", s_pmu.getChipID());

    // The board has no battery temperature sensor. Leaving TS measurement
    // enabled can make charging behave abnormally.
    s_pmu.disableTSPinMeasure();
    s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    s_pmu.enableCellbatteryCharge();
    s_pmu.setLowBatWarnThreshold(15);
    s_pmu.setLowBatShutdownThreshold(5);
    s_pmu.setOnLevel(1); // 512 ms power-key press to power on.

    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();
    s_pmu.clearIrqStatus();

    s_pmu_ready = true;
    power_manager_update();
    ESP_LOGI(TAG,
             "battery=%s charging=%s vbus=%s",
             s_status.battery_present ? "yes" : "no",
             s_status.charging ? "yes" : "no",
             s_status.vbus_present ? "yes" : "no");
    return ESP_OK;
}

void power_manager_update(void)
{
    if (!s_pmu_ready) {
        return;
    }

    power_status_t next = {};
    next.initialized = true;
    next.battery_present = s_pmu.isBatteryConnect();
    next.charging = s_pmu.isCharging();
    next.discharging = s_pmu.isDischarge();
    next.vbus_present = s_pmu.isVbusIn() || s_pmu.isVbusGood();
    next.battery_mv = next.battery_present ? s_pmu.getBattVoltage() : 0;
    next.vbus_mv = s_pmu.getVbusVoltage();
    next.system_mv = s_pmu.getSystemVoltage();

    int percent = next.battery_present ? s_pmu.getBatteryPercent() : 0;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    next.battery_percent = (uint8_t)percent;

    const xpowers_chg_status_t charger_status = s_pmu.getChargerStatus();
    next.full = charger_status == XPOWERS_AXP2101_CHG_DONE_STATE ||
                (next.vbus_present && next.battery_present && !next.charging &&
                 next.battery_percent >= 99);

    portENTER_CRITICAL(&s_lock);
    s_status = next;
    portEXIT_CRITICAL(&s_lock);
}

void power_manager_get_status(power_status_t *status)
{
    if (status == nullptr) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t power_manager_shutdown(void)
{
    if (!s_pmu_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pmu.shutdown();
    return ESP_OK;
}
