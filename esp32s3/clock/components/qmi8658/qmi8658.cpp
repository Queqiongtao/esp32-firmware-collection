#include "qmi8658.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "qmi8658";

static constexpr uint8_t REG_WHO_AM_I = 0x00;
static constexpr uint8_t REG_CTRL1 = 0x02;
static constexpr uint8_t REG_CTRL2 = 0x03;
static constexpr uint8_t REG_CTRL3 = 0x04;
static constexpr uint8_t REG_CTRL7 = 0x08;
static constexpr uint8_t REG_TEMP_L = 0x33;

static constexpr uint8_t CTRL1_ADDR_AUTO_INCREMENT = BIT(6);
static constexpr uint8_t CTRL1_SPI_BIG_ENDIAN = BIT(5);

static constexpr uint8_t CTRL2_ACC_RANGE_8G = 0x20;
static constexpr uint8_t CTRL2_ACC_ODR_125HZ = 0x06;
static constexpr uint8_t CTRL3_GYR_RANGE_512DPS = 0x50;
static constexpr uint8_t CTRL3_GYR_ODR_125HZ = 0x06;
static constexpr uint8_t CTRL7_ACC_ENABLE = BIT(0);
static constexpr uint8_t CTRL7_GYR_ENABLE = BIT(1);

static constexpr float ACCEL_LSB_PER_G_8G = 4096.0f;
static constexpr float GYRO_LSB_PER_DPS_512 = 64.0f;
static constexpr float TEMP_LSB_PER_DEG_C = 256.0f;

static int16_t le16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static esp_err_t write_reg(qmi8658_t *imu, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(imu->dev_handle, data, sizeof(data), imu->timeout_ms);
}

static esp_err_t read_regs(qmi8658_t *imu, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(
        imu->dev_handle, &reg, sizeof(reg), data, len, imu->timeout_ms);
}

static esp_err_t add_device(qmi8658_t *imu,
                            i2c_master_bus_handle_t bus_handle,
                            uint16_t address,
                            uint32_t scl_speed_hz)
{
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = address;
    dev_config.scl_speed_hz = scl_speed_hz;

    return i2c_master_bus_add_device(bus_handle, &dev_config, &imu->dev_handle);
}

static esp_err_t detect_device(qmi8658_t *imu, const qmi8658_config_t *config)
{
    const uint16_t addresses[] = {
        config->device_address != 0 ?
            config->device_address : static_cast<uint16_t>(QMI8658_I2C_ADDRESS_LOW),
        QMI8658_I2C_ADDRESS_HIGH,
    };

    for (uint16_t address : addresses) {
        if (address == 0) {
            continue;
        }

        if (i2c_master_probe(config->bus_handle, address, config->timeout_ms) != ESP_OK) {
            continue;
        }

        esp_err_t err = add_device(imu, config->bus_handle, address, config->scl_speed_hz);
        if (err != ESP_OK) {
            return err;
        }

        uint8_t chip_id = 0;
        err = read_regs(imu, REG_WHO_AM_I, &chip_id, sizeof(chip_id));
        if (err != ESP_OK) {
            return err;
        }

        imu->device_address = address;
        imu->chip_id = chip_id;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t qmi8658_init(qmi8658_t *imu, const qmi8658_config_t *config)
{
    ESP_RETURN_ON_FALSE(imu != NULL, ESP_ERR_INVALID_ARG, TAG, "imu is null");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "bus handle is null");
    ESP_RETURN_ON_FALSE(config->scl_speed_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid scl speed");

    memset(imu, 0, sizeof(*imu));
    imu->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : QMI8658_DEFAULT_TIMEOUT_MS;
    imu->accel_lsb_per_g = ACCEL_LSB_PER_G_8G;
    imu->gyro_lsb_per_dps = GYRO_LSB_PER_DPS_512;

    qmi8658_config_t normalized_config = *config;
    normalized_config.timeout_ms = imu->timeout_ms;

    ESP_RETURN_ON_ERROR(detect_device(imu, &normalized_config), TAG, "QMI8658 not found");

    ESP_RETURN_ON_ERROR(
        write_reg(imu, REG_CTRL1, CTRL1_ADDR_AUTO_INCREMENT | CTRL1_SPI_BIG_ENDIAN),
        TAG, "write CTRL1 failed");
    ESP_RETURN_ON_ERROR(
        write_reg(imu, REG_CTRL2, CTRL2_ACC_RANGE_8G | CTRL2_ACC_ODR_125HZ),
        TAG, "write CTRL2 failed");
    ESP_RETURN_ON_ERROR(
        write_reg(imu, REG_CTRL3, CTRL3_GYR_RANGE_512DPS | CTRL3_GYR_ODR_125HZ),
        TAG, "write CTRL3 failed");
    ESP_RETURN_ON_ERROR(
        write_reg(imu, REG_CTRL7, CTRL7_ACC_ENABLE | CTRL7_GYR_ENABLE),
        TAG, "write CTRL7 failed");

    ESP_LOGI(TAG, "QMI8658 initialized at 0x%02X, chip id 0x%02X",
             imu->device_address, imu->chip_id);

    return ESP_OK;
}

esp_err_t qmi8658_read(qmi8658_t *imu, qmi8658_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(imu != NULL && imu->dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "imu is invalid");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG, "sample is null");

    uint8_t data[14] = {};
    ESP_RETURN_ON_ERROR(read_regs(imu, REG_TEMP_L, data, sizeof(data)), TAG, "read sample failed");

    memset(sample, 0, sizeof(*sample));
    sample->raw_temperature = le16(&data[0]);
    sample->raw_accel[0] = le16(&data[2]);
    sample->raw_accel[1] = le16(&data[4]);
    sample->raw_accel[2] = le16(&data[6]);
    sample->raw_gyro[0] = le16(&data[8]);
    sample->raw_gyro[1] = le16(&data[10]);
    sample->raw_gyro[2] = le16(&data[12]);

    sample->temperature_c = sample->raw_temperature / TEMP_LSB_PER_DEG_C;
    for (int i = 0; i < 3; ++i) {
        sample->accel_g[i] = sample->raw_accel[i] / imu->accel_lsb_per_g;
        sample->gyro_dps[i] = sample->raw_gyro[i] / imu->gyro_lsb_per_dps;
    }

    return ESP_OK;
}
