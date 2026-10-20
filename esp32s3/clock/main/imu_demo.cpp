#include "imu_demo.h"

#include <math.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"
#include "spring_angle.h"

static const char *TAG = "ImuDemo";

static constexpr int IMU_SAMPLE_HZ = 100;
static constexpr int IMU_SAMPLE_PERIOD_MS = 1000 / IMU_SAMPLE_HZ;
static constexpr int IMU_ERROR_LOG_PERIOD_MS = 2000;
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
static imu_demo_angle_cb_t s_angle_cb = NULL;
static void *s_angle_user_ctx = NULL;
static float s_clock_angle_filtered = 0.0f;
static bool s_clock_angle_valid = false;
static volatile bool s_imu_active = true;

static void update_clock_angle(const qmi8658_accel_sample_t *sample)
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

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_error_log = 0;
    bool error_logged = false;
    while (true) {
        if (!s_imu_active) {
            s_clock_angle_valid = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            last_wake = xTaskGetTickCount();
            continue;
        }

        qmi8658_accel_sample_t sample = {};
        esp_err_t err = qmi8658_read_accel(imu, &sample);
        if (err == ESP_OK) {
            update_clock_angle(&sample);
            error_logged = false;
        } else {
            const TickType_t now = xTaskGetTickCount();
            if (!error_logged ||
                (now - last_error_log) >= pdMS_TO_TICKS(IMU_ERROR_LOG_PERIOD_MS)) {
                ESP_LOGW(TAG, "Failed to read QMI8658: %s", esp_err_to_name(err));
                last_error_log = now;
                error_logged = true;
            }
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
    s_imu_active = true;

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
        .enable_gyro = false,
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

void imu_demo_set_active(bool active)
{
    s_imu_active = active;
}
