/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "display_config.h"
#include "esp_heap_caps.h"

#define TEST_AUDIO_BYTES_PER_SAMPLE             2
#define TEST_AUDIO_CHANNELS                     2

/* ---------------------------- SD card (SDMMC) ------------------------------ *
 * Board wiring (JC1060P470, same as ESP32-P4 Function EV Board):
 *   D0-D3 = GPIO39-42, CLK = GPIO43, CMD = GPIO44
 *
 * These are SLOT_0 IO MUX dedicated UHS-I pins, connected directly to the
 * SDMMC controller (not through the GPIO matrix). SDMMC IO power is supplied
 * by the on-chip LDO channel 4 (LDO_VO4). Channel 3 is occupied by the MIPI
 * DSI PHY (2.5V). */

#define TEST_SDCARD_PIN_CLK                     GPIO_NUM_43
#define TEST_SDCARD_PIN_CMD                     GPIO_NUM_44
#define TEST_SDCARD_PIN_D0                      GPIO_NUM_39
#define TEST_SDCARD_PIN_D1                      GPIO_NUM_40
#define TEST_SDCARD_PIN_D2                      GPIO_NUM_41
#define TEST_SDCARD_PIN_D3                      GPIO_NUM_42

#define TEST_SDCARD_BUS_WIDTH                   4

#define TEST_SDCARD_PWR_LDO_CHAN                4

#define TEST_SDCARD_MOUNT_POINT                 "/sdcard"
#define TEST_SDCARD_MAX_FILES                   5
#define TEST_SDCARD_ALLOCATION_UNIT_SIZE        (64 * 1024)

/* ---------------------------- RTC (RX8025T-UC) ---------------------------- *
 * Board wiring:
 *   RX8025T-UC RTC borrows the board-owned shared I2C bus. The bus port,
 *   pins, and clock are defined once in board_config.h. */

#define TEST_RTC_CHIP_NAME                      "RX8025T-UC"
#define TEST_RTC_I2C_ADDR                       0x32
#define TEST_RTC_I2C_TIMEOUT_MS                 100
#define TEST_RTC_READ_INTERVAL_MS               5000

/* ---------------------------- Audio (I2S + ES8311) ------------------------- *
 * Board wiring (JC1060P470, same as ESP32-P4 Function EV Board, FIB type):
 *   I2S MCLK = GPIO13, BCLK = GPIO12, WS = GPIO10
 *   I2S DOUT = GPIO9 (-> ES8311), DIN = GPIO11 (<- ES8311)
 *   PA (power amplifier enable) = GPIO20
 *
 * ES8311 codec borrows the board-owned shared I2C bus via board_i2c. */

#define TEST_AUDIO_I2S_NUM                      0
#define TEST_AUDIO_PIN_MCLK                     GPIO_NUM_13
#define TEST_AUDIO_PIN_BCLK                     GPIO_NUM_12
#define TEST_AUDIO_PIN_WS                       GPIO_NUM_10
#define TEST_AUDIO_PIN_DOUT                     GPIO_NUM_9
#define TEST_AUDIO_PIN_DIN                      GPIO_NUM_11
#define TEST_AUDIO_PIN_PA                       GPIO_NUM_20

#define TEST_AUDIO_SAMPLE_RATE                  16000
#define TEST_AUDIO_MCLK_MULTIPLE                384
#define TEST_AUDIO_RECV_BUF_SIZE                (2400)
#define TEST_AUDIO_RECORD_SECONDS               5
#define TEST_AUDIO_RECORD_BUF_SIZE              (TEST_AUDIO_SAMPLE_RATE * TEST_AUDIO_BYTES_PER_SAMPLE * \
                                                 TEST_AUDIO_CHANNELS * TEST_AUDIO_RECORD_SECONDS)
#define TEST_AUDIO_TASK_STACK_SIZE              4096
#define TEST_AUDIO_TASK_PRIORITY                4
#define TEST_AUDIO_TASK_CORE_ID                 tskNO_AFFINITY
#ifdef CONFIG_EXAMPLE_VOICE_VOLUME
#define TEST_AUDIO_VOICE_VOLUME                 CONFIG_EXAMPLE_VOICE_VOLUME
#else
#define TEST_AUDIO_VOICE_VOLUME                 60
#endif
#ifdef CONFIG_EXAMPLE_MIC_GAIN
#define TEST_AUDIO_MIC_GAIN                     CONFIG_EXAMPLE_MIC_GAIN
#else
#define TEST_AUDIO_MIC_GAIN                     3
#endif

/* ---------------------------- Battery monitor ------------------------------ *
 * Board wiring:
 *   BAT -> 68K -> GPIO52(sample point) -> 100K -> GND
 *
 * The ADC sees the divider midpoint. Battery voltage is recovered in software
 * with: Vbat = Vadc * (68K + 100K) / 100K = Vadc * 1.68
 *
 * The first-stage percent estimate is only for demo observation. It linearly
 * maps 3.3V to 0% and 4.2V to 100%; it is not a precision fuel gauge. */

#define TEST_BATTERY_ADC_UNIT                    ADC_UNIT_2
#define TEST_BATTERY_ADC_CHANNEL                 ADC_CHANNEL_3
#define TEST_BATTERY_ADC_GPIO                    52
#define TEST_BATTERY_ADC_ATTEN                   ADC_ATTEN_DB_12
#define TEST_BATTERY_ADC_BITWIDTH                ADC_BITWIDTH_DEFAULT
#define TEST_BATTERY_ADC_RAW_FULL_SCALE          4095
#define TEST_BATTERY_ADC_FALLBACK_REF_MV         3300

#define TEST_BATTERY_DIVIDER_R_TOP_OHM           68000
#define TEST_BATTERY_DIVIDER_R_BOTTOM_OHM        100000

#define TEST_BATTERY_SAMPLE_COUNT                16
#define TEST_BATTERY_UPDATE_PERIOD_MS            1000
#define TEST_BATTERY_EMPTY_MV                    3300
#define TEST_BATTERY_FULL_MV                     4200

/* ---------------------------- USB ports overview --------------------------- *
 * Board wiring (JC1060P470, V1.0 chip, FIB type, two USB Type-C ports):
 *
 *   FULL USB (丝印) -> GPIO24(DM)/GPIO25(DP) -> USB_INT_PHY0
 *     This is the USB Serial/JTAG (USJ) controller: a fixed-function USB device
 *     exposing a CDC-ACM serial port (firmware download + IDF log console).
 *     It enumerates automatically at boot; no application code is required.
 *     The USJ controller is NOT a DWC OTG and cannot run TinyUSB custom device
 *     stacks or USB Host. Use usb_serial_jtag_driver_install() for custom
 *     byte-stream traffic over the fixed CDC-ACM interface.
 *
 *   HIGH USB (丝印) -> package pins 49/50 -> internal UTMI High-Speed PHY
 *     (controller-independent dedicated pins, NOT in the GPIO matrix).
 *     This is DWC OTG controller[0]; supports HS device (TinyUSB) and HS host
 *     (esp-usb usb_host). controller[0] can be only one role at a time.
 *
 * Note: The original comments here mistakenly labeled "High-Speed USB Device:
 *   GPIO24/25". On ESP32-P4 V1.0 (per IDF io_mux_reg.h hw_ver1) GPIO24/25 are
 *   USB_INT_PHY0 = USJ, while the HS UTMI PHY has no GPIO number. This block
 *   was corrected while adding the UVC host module. The old FS OTG reference
 *   to GPIO26/27 (USB_INT_PHY1, controller[1]) is intentionally omitted: that
 *   route conflicts with DISPLAY_PIN_NUM_LCD_RST (GPIO27) and is unsuitable
 *   for this board.
 *
 * CDC-ACM demo (test_usb.c, TinyUSB):
 *   Uses the HIGH USB port as a TinyUSB HS device. It defaults to off
 *   (EXAMPLE_ENABLE_TEST_USB=n) because the same controller[0] is needed by
 *   the UVC camera host module. Enable one or the other, not both. */

#define TEST_USB_CDC_PORT                       TINYUSB_CDC_ACM_0
#define TEST_USB_CDC_EVENT_QUEUE_LEN            16
#define TEST_USB_CDC_RX_BUF_SIZE                64
#define TEST_USB_CDC_TX_FLUSH_TIMEOUT_TICKS     0
#define TEST_USB_CDC_TASK_STACK_SIZE            4096
#define TEST_USB_CDC_TASK_PRIORITY              4
#define TEST_USB_CDC_TASK_CORE_ID               tskNO_AFFINITY

/* ---------------------------- Wireless (ESP32-C6 hosted) ------------------ *
 * Board wiring:
 *   ESP32-P4 <-> ESP32-C6 SDIO
 *   D0 = GPIO14, D1 = GPIO15, D2 = GPIO16, D3 = GPIO17, CLK = GPIO18, CMD = GPIO19
 *
 * Current software status:
 *   - WiFi station test path is brought up through esp_hosted + esp_wifi_remote
 *   - BLE Peripheral smoke test uses hosted NimBLE over ESP-Hosted VHCI */

#define TEST_WIRELESS_SDIO_PIN_D0               GPIO_NUM_14
#define TEST_WIRELESS_SDIO_PIN_D1               GPIO_NUM_15
#define TEST_WIRELESS_SDIO_PIN_D2               GPIO_NUM_16
#define TEST_WIRELESS_SDIO_PIN_D3               GPIO_NUM_17
#define TEST_WIRELESS_SDIO_PIN_CLK              GPIO_NUM_18
#define TEST_WIRELESS_SDIO_PIN_CMD              GPIO_NUM_19

#define TEST_WIRELESS_SDIO_BUS_WIDTH            4
#define TEST_WIRELESS_SDIO_CLOCK_KHZ            25000
#define TEST_WIRELESS_CMD_TIMEOUT_MS            1000
#define TEST_WIRELESS_WIFI_SCAN_TIMEOUT_MS      10000
#define TEST_WIRELESS_WIFI_CONNECT_RETRY        3
#define TEST_WIRELESS_BLE_SCAN_TIME_MS          5000

/* ---------------------------- USB UVC camera (High-Speed Host) ------------- *
 * Board wiring (JC1060P470, V1.0 chip, FIB type):
 *   UVC 摄像头接到 HIGH USB 口（丝印），该口走 ESP32-P4 内部 UTMI High-Speed
 *   PHY 的专用封装脚（49/50），对应 DWC OTG controller[0]，480Mbps。
 *   USB Host Library 通过 usb_host_config_t.peripheral_map 选择控制器：
 *     peripheral_map = BIT0  -> HS OTG（controller[0]，走 UTMI HS PHY，本模块）
 *     peripheral_map = BIT1  -> FS OTG（controller[1]，内部 FS PHY=GPIO26/27）
 *   本模块用 BIT0：480Mbps 带宽可上 1280x720，且 UTMI PHY 不进 GPIO 矩阵，
 *   不会触碰 DISPLAY_PIN_NUM_LCD_RST(GPIO27)，避免 FS OTG 路径导致的黑屏。
 *   原理图未提供由 ESP32-P4 GPIO 控制的 HIGH USB VBUS load switch；
 *   usb_host_lib_set_root_port_power(false) 只关闭 DWC root port，不能保证摄像头
 *   5V 真正掉电。软件恢复耗尽后仍可能需要重新插拔摄像头。
 *
 *   controller[0] 同一时刻只能当 device 或 host。因此本模块启用时，CDC-ACM
 *   demo（test_usb.c，TinyUSB HS device）不可同时启用。CDC 功能改由 FULL USB
 *   口的 USJ 固化 CDC-ACM 串口承担（见上文 USB ports overview）。
 *
 * 当前路径：USB Host + UVC 收流 + 硬件 JPEG 解码（RGB565），再由 PPA
 *   或 CPU 等比缩放到固定预览缓冲。UI 通过 test_uvc_copy_preview() 复制
 *   快照，不直接持有 USB、JPEG 或 PPA 的内部缓冲。
 *   流格式（分辨率/FPS）在设备连接时从描述符枚举后动态协商，优先
 *   匹配 TEST_UVC_PREFER_H_RES × TEST_UVC_PREFER_V_RES 的 MJPEG +
 *   最高 fps，不匹配时降级到设备实际支持的最大 MJPEG 格式。
 *
 * Cache 一致性：
 *   - UVC 帧缓冲在 PSRAM，由 USB-DWC DMA 写入，应用 CPU 读 frame->data；
 *     esp-usb UVC 驱动内部已处理该 DMA->CPU 方向的 cache invalidate，
 *     应用层无需额外 esp_cache_msync。
 *   - JPEG/PPA 驱动负责各自 DMA 边界；应用层对 CPU 清零、CPU fallback
 *     缩放等写入的预览 PSRAM 显式执行 esp_cache_msync(DIR_C2M)。
 *   - UI 复制到 LVGL image 自有缓冲后同样显式写回 cache，再通知 LVGL
 *     图像失效，遵循 AGENTS.md cache 一致性约束。 */

#define TEST_UVC_USB_HOST_PERIPHERAL_MAP       (1U << 0)   /* HS OTG: controller[0], UTMI HS PHY, HIGH USB port */
#define TEST_UVC_VID                            UVC_HOST_ANY_VID
#define TEST_UVC_PID                            UVC_HOST_ANY_PID
#define TEST_UVC_UVC_STREAM_INDEX              0

#define TEST_UVC_FORMAT                         UVC_VS_FORMAT_MJPEG

/* Preferred resolution for stream negotiation. Not enforced: if the device
 * does not support this exact resolution, the module falls back to the
 * device's actual maximum supported MJPEG resolution. */
#define TEST_UVC_PREFER_H_RES                   1920
#define TEST_UVC_PREFER_V_RES                   1080

/* Maximum number of frame descriptors the module will enumerate from a single
 * UVC device via uvc_host_get_frame_list(). Must be >= the device's
 * frame_info_num for the selected stream. 24 leaves headroom for cameras that
 * expose several resolutions and frame intervals. */
#define TEST_UVC_FRAME_INFO_MAX                 24

#define TEST_UVC_FRAME_BUF_COUNT                3
#define TEST_UVC_FRAME_SIZE                     0          /* 0: use dwMaxVideoFrameSize from negotiation */
#define TEST_UVC_FRAME_HEAP_CAPS                MALLOC_CAP_SPIRAM
/* ISOC URB 缓冲: 每个 URB 容纳 urb_size/MPS(1024) 个等时包, 每包对应一个
 * 125us microframe。4 x 20KB = 80 个包 = 10ms 缓冲(占内部 DMA RAM 80KB)。
 * 注意 HCD(hcd_dwc.c) 的 ISOC qTD 环只有 64 槽且每管道仅双缓冲调度:
 * 单个 URB 不得超过 61 包, URB 总深度也不能盲目加大 —— 实测 8x20KB
 * 会导致 ISOC 调度绕死、一帧不收(并进一步诱发 CTRL 超时和 usbh 断言
 * 崩溃), 4x20KB 为实证可用配置, 请勿在未上板验证的情况下加深。 */
#define TEST_UVC_NUMBER_OF_URBS                 4
#define TEST_UVC_URB_SIZE                       (20 * 1024)

/* MJPEG 帧尾 EOI(FFD9) 搜索窗口: 部分摄像头在 EOI 后填充 padding 字节,
 * 完整性校验从帧尾向前在该窗口内查找 EOI。 */
#define TEST_UVC_MJPEG_EOI_SEARCH_WINDOW        512

#define TEST_UVC_FRAME_QUEUE_LEN                3

#define TEST_UVC_USB_LIB_TASK_STACK             4096
#define TEST_UVC_USB_LIB_TASK_PRIORITY          10
#define TEST_UVC_USB_LIB_TASK_CORE              1

#define TEST_UVC_DRIVER_TASK_STACK               4096
#define TEST_UVC_DRIVER_TASK_PRIORITY            11
#define TEST_UVC_DRIVER_TASK_CORE               1

#define TEST_UVC_WORKER_TASK_STACK              8192
#define TEST_UVC_WORKER_TASK_PRIORITY           4
#define TEST_UVC_WORKER_TASK_CORE               1

#define TEST_UVC_ENUM_MAX_ATTEMPTS              3
#define TEST_UVC_ENUM_TIMEOUT_MS                2000
#define TEST_UVC_ROOT_PORT_CYCLE_DELAY_MS       500
#define TEST_UVC_DEVICE_READY_DELAY_MS          1000
#define TEST_UVC_STREAM_OPEN_MAX_ATTEMPTS       3
#define TEST_UVC_STREAM_OPEN_RETRY_DELAY_MS     500
#define TEST_UVC_FRAME_RECV_TIMEOUT_MS          5000
#define TEST_UVC_STREAM_STALL_TIMEOUT_MS         5000
#define TEST_UVC_RECOVERY_MAX_ATTEMPTS           3
#define TEST_UVC_RECOVERY_POLL_MS                250

#define TEST_UVC_JPEG_ENGINE_TIMEOUT_MS         200        /* > 33ms (30fps 上限), 给硬件解码容限 */
#define TEST_UVC_JPEG_DECODE_TIMEOUT_MS         500
#define TEST_UVC_JPEG_QUEUE_LIMIT              0          /* 0: 不限制连续解码错误日志频率, 由计数器自行管控 */

/* 固定大小的 RGB565 预览帧。UVC worker 将已解码的原始画面等比缩放并居中
 * 到此缓冲；UI 通过受控拷贝取得快照，避免直接持有 JPEG DMA 输出缓冲。
 * 全屏预览: 与 MIPI-DSI 面板同尺寸, 非同比例源等比缩放后
 * 上下/左右留黑边。 */
#define TEST_UVC_PREVIEW_H_RES                  DISPLAY_PANEL_H_RES
#define TEST_UVC_PREVIEW_V_RES                  DISPLAY_PANEL_V_RES
#define TEST_UVC_PREVIEW_REFRESH_MS             100
/* worker 更新预览的最小间隔, 按协商的源分辨率自适应。1080p 下 PPA 缩放
 * 需整读 ~4MB PSRAM 源 (实测 preview 耗时 90ms), 与 JPEG 解码/显示刷新
 * 争用 PSRAM 带宽, 会拉长 ISOC 回调 memcpy 进而诱发丢包截断帧, 故大
 * 分辨率保持保守间隔; 小分辨率源 PSRAM 读取量小, 可提高预览频率。
 * 分级值未经全量上板验证, 若出现截断帧计数上升需回调保守。 */
#define TEST_UVC_PREVIEW_UPDATE_INTERVAL_SMALL_MS  100  /* 源 <= 640x480, ~10fps */
#define TEST_UVC_PREVIEW_UPDATE_INTERVAL_MEDIUM_MS 100  /* 源 <= 1280x720, ~10fps */
#define TEST_UVC_PREVIEW_UPDATE_INTERVAL_LARGE_MS  200  /* 更大源 (1080p), 实证保守值 */
#define TEST_UVC_PPA_BUFFER_ALIGNMENT           128
