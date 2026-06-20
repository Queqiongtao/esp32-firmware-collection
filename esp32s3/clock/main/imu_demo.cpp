#include "imu_demo.h"

#include <math.h>

#include "attitude.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"
#include "spring_angle.h"

static const char *TAG = "ImuDemo";

static constexpr int IMU_SAMPLE_HZ = 100;
static constexpr int IMU_SAMPLE_PERIOD_MS = 1000 / IMU_SAMPLE_HZ;
static constexpr float IMU_SAMPLE_DT_S = 1.0f / IMU_SAMPLE_HZ;
static constexpr int GYRO_BIAS_CALIBRATION_SAMPLES = 200;
static constexpr int ATTITUDE_LOG_PERIOD_MS = 500;
static constexpr float RAD_TO_DEG = 57.29577951308232f;

// Continuous in-plane gravity angle: angle = OFFSET - SIGN * atan2(ax, ay).
// Defaults map the four upright orientations to the previous 270/0/90/180 targets.
// Flip CLOCK_ANGLE_SIGN to -1.0f if the clock rotates the wrong way on hardware.
static constexpr float CLOCK_ANGLE_OFFSET_DEG = 90.0f;
static constexpr float CLOCK_ANGLE_SIGN = 1.0f;
// Below this horizontal gravity magnitude the screen is near-flat and the
// in-plane angle is meaningless, so the last valid angle is frozen.
static constexpr float CLOCK_ANGLE_FLAT_MIN_G = 0.3f;
// First-order IIR smoothing on the raw angle (per 100 Hz sample).
static constexpr float CLOCK_ANGLE_LPF_ALPHA = 0.12f;

static qmi8658_t s_imu = {};
static attitude_t s_attitude = {};
static imu_demo_angle_cb_t s_angle_cb = NULL;
static void *s_angle_user_ctx = NULL;
static float s_clock_angle_filtered = 0.0f;
static bool s_clock_angle_valid = false;

static esp_err_t calibrate_gyro_bias(qmi8658_t *imu, float gyro_bias_dps[3])
{
    gyro_bias_dps[0] = 0.0f;
    gyro_bias_dps[1] = 0.0f;
    gyro_bias_dps[2] = 0.0f;

    ESP_LOGI(TAG, "Keep board still, calibrating gyro bias...");

    int valid_samples = 0;
    TickType_t last_wake = xTaskGetTickCount();
    for (int i = 0; i < GYRO_BIAS_CALIBRATION_SAMPLES; ++i) {
        qmi8658_sample_t sample = {};
        esp_err_t err = qmi8658_read(imu, &sample);
        if (err == ESP_OK) {
            gyro_bias_dps[0] += sample.gyro_dps[0];
            gyro_bias_dps[1] += sample.gyro_dps[1];
            gyro_bias_dps[2] += sample.gyro_dps[2];
            ++valid_samples;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
    }

    if (valid_samples == 0) {
        return ESP_FAIL;
    }

    gyro_bias_dps[0] /= valid_samples;
    gyro_bias_dps[1] /= valid_samples;
    gyro_bias_dps[2] /= valid_samples;

    ESP_LOGI(TAG, "Gyro bias[dps] x=%.3f y=%.3f z=%.3f",
             gyro_bias_dps[0], gyro_bias_dps[1], gyro_bias_dps[2]);

    return ESP_OK;
}

static void update_clock_angle(const qmi8658_sample_t *sample)
{
    if (s_angle_cb == NULL) {
        return;
    }

    const float ax = sample->accel_g[0];
    const float ay = sample->accel_g[1];
    const float horizontal_g = sqrtf(ax * ax + ay * ay);
    if (horizontal_g < CLOCK_ANGLE_FLAT_MIN_G) {
        return;
    }

    const float raw = spring_angle_normalize(
        CLOCK_ANGLE_OFFSET_DEG - CLOCK_ANGLE_SIGN * atan2f(ax, ay) * RAD_TO_DEG);

    if (!s_clock_angle_valid) {
        s_clock_angle_filtered = raw;
        s_clock_angle_valid = true;
    } else {
        const float delta = spring_angle_shortest_delta(raw, s_clock_angle_filtered);
        s_clock_angle_filtered =
            spring_angle_normalize(s_clock_angle_filtered + CLOCK_ANGLE_LPF_ALPHA * delta);
    }

    s_angle_cb(s_clock_angle_filtered, s_angle_user_ctx);
}

static void imu_demo_task(void *arg)
{
    qmi8658_t *imu = static_cast<qmi8658_t *>(arg);
    float gyro_bias_dps[3] = {};

    attitude_init(&s_attitude, NULL);

    if (calibrate_gyro_bias(imu, gyro_bias_dps) != ESP_OK) {
        ESP_LOGW(TAG, "Gyro bias calibration failed, using zero bias");
    }

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_log = last_wake;
    while (true) {
        qmi8658_sample_t sample = {};
        esp_err_t err = qmi8658_read(imu, &sample);
        if (err == ESP_OK) {
            update_clock_angle(&sample);
            TickType_t now = xTaskGetTickCount();

            float gyro_dps[3] = {
                sample.gyro_dps[0] - gyro_bias_dps[0],
                sample.gyro_dps[1] - gyro_bias_dps[1],
                sample.gyro_dps[2] - gyro_bias_dps[2],
            };

            attitude_update(&s_attitude, sample.accel_g, gyro_dps, IMU_SAMPLE_DT_S);

            if ((now - last_log) >= pdMS_TO_TICKS(ATTITUDE_LOG_PERIOD_MS)) {
                attitude_angles_t angles = {};
                attitude_get_angles(&s_attitude, &angles);
                ESP_LOGI(TAG,
                         "attitude[deg] roll=%.2f pitch=%.2f yaw=%.2f clock_angle=%.2f",
                         angles.roll_deg, angles.pitch_deg, angles.yaw_deg,
                         s_clock_angle_filtered);
                last_log = now;
            }
        } else {
            ESP_LOGW(TAG, "Failed to read QMI8658: %s", esp_err_to_name(err));
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
    }
}

esp_err_t imu_demo_start(imu_demo_angle_cb_t angle_cb,
                         void *angle_user_ctx)
{
    s_angle_cb = angle_cb;
    s_angle_user_ctx = angle_user_ctx;
    s_clock_angle_valid = false;

    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    if (bus_handle == NULL) {
        ESP_LOGW(TAG, "I2C bus is unavailable");
        return ESP_FAIL;
    }

    const qmi8658_config_t config = {
        .bus_handle = bus_handle,
        .device_address = 0,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
        .timeout_ms = QMI8658_DEFAULT_TIMEOUT_MS,
    };

    esp_err_t err = qmi8658_init(&s_imu, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t task_created = xTaskCreate(
        imu_demo_task,
        "imu_demo",
        4096,
        &s_imu,
        4,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGW(TAG, "Failed to create IMU demo task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
