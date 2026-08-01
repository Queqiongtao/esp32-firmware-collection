# 依赖与构建

最后更新：2026-08-01

## 构建入口

本工程使用 `dev.sh`（通过 `eim` 调用 ESP-IDF），必须在本工程目录内运行：

```bash
./dev.sh build
./dev.sh ports
./dev.sh -p /dev/ttyACM0 flash-monitor
./dev.sh menuconfig
```

- 默认串口 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=...` 覆盖
- `eim` 日志默认 `/tmp/eim-amoled_fluid.log`，可用 `EIM_LOG_FILE` 覆盖
- 首次构建会经 ESP-IDF 组件管理器下载 `managed_components/`（需网络），之后离线构建
- `build/`、`managed_components/`、`dependencies.lock`、`sdkconfig` 等由仓库根 `.gitignore` 忽略，不入库

## 组件依赖（`components/board_amoled_1_8/idf_component.yml`）

> 说明：参考工程 `waveshare_amoled_1_8` 的清单里曾带 `waveshare/esp32_s3_touch_amoled_1_8`（Waveshare BSP），但本工程板级代码实际只走本地 SH8601 驱动路径，从未调用 BSP API；BSP 还会带来未验证的构建依赖。本工程改为显式声明代码实际使用的 `waveshare/esp_lcd_sh8601` 与触摸组件（与 `companion_display/firmware/esp32s3/components/board_amoled_1_8` 的已验证清单一致）。

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| `waveshare/esp_lcd_sh8601` | ^2 | SH8601 面板驱动（当前为 2.0.0） |
| `espressif/esp_lcd_touch` | ^1 | LCD 触摸抽象层 |
| `espressif/esp_lcd_touch_ft5x06` | ^1 | FT3168 兼容驱动 |
| `espressif/esp_io_expander_tca9554` | ^2 | TCA9554 扩展 IO |
| `espressif/esp_codec_dev` | ~1.5 | ES8311 音频编解码设备 |
| `espressif/usb` | ^1 | USB 支持（保留） |
| `espp/pcf85063` | ^1.1.4 | RTC 驱动 |
| `esp-idf-lib/qmi8658c` | ^0.5.7 | IMU 驱动 |
| `cube32esp/xpowerslib` | ^0.3.3 | AXP2101 PMU 驱动 |

组件内 `REQUIRES` 还包含：`esp_driver_*`、`esp_lcd`、`espressif__esp_io_expander`、`waveshare__esp_lcd_sh8601`、`fatfs`、`sdmmc`、`esp_codec_dev`。

## 离线构建说明（本机无网络时）

首次构建需要联网解依赖；若环境无网络，可用以下方式复用本机已有缓存（构建产物，均不入库）：

1. 从同依赖工程复制缓存与锁文件：

```bash
cp -r ../companion_display/firmware/esp32s3/managed_components managed_components
cp ../companion_display/firmware/esp32s3/dependencies.lock dependencies.lock
```

2. 用 ESP-IDF 组件管理器计算本工程 `manifest_hash` 并替换锁文件中的值，同时从 `direct_dependencies` 删除本工程没有的条目（例如 `espressif/esp_jpeg`）。
3. 构建时关闭“检查新版本”避免联网探测：

```bash
IDF_COMPONENT_CHECK_NEW_VERSION=0 ./dev.sh build
```

2026-08-01 首次构建即按此流程完成，构建产物正常生成。

## sdkconfig.defaults 关键配置

### 芯片与系统

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `CONFIG_IDF_TARGET` | esp32s3 | |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` | y | |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | |
| `CONFIG_SPIRAM` / `CONFIG_SPIRAM_MODE_OCT` | y / y | 八线 PSRAM |
| `CONFIG_SPIRAM_SPEED_80M` | y | |
| `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `RODATA` / `XIP_FROM_PSRAM` | y | 代码/常量放 PSRAM 的许可 |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` | y | |
| `CONFIG_ESP_CONSOLE_UART_BAUDRATE` | 2000000 | 2M 波特率控制台 |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | y | 性能优化 |

### 板级 pinout（`CONFIG_BOARD_AMOLED_1_8_*`）

| 配置 | 值 |
| --- | --- |
| I2C port / SDA / SCL / 频率 | 0 / 15 / 14 / 400000 |
| LCD CS / PCLK / D0-D3 / TE | 12 / 11 / 4,5,6,7 / 13 |
| LCD RST / POWER EXIO | EXIO0 / EXIO1（active high） |
| LCD 分辨率 / SPI host | 368x448 / 2 |
| Touch INT / RST EXIO | 21 / EXIO2 |
| SD D0 / CMD / CLK | 3 / 1 / 2 |
| I2S MCLK / BCLK / LRCK / DOUT / DIN | 16 / 9 / 45 / 8 / 10 |
| PA enable | 46 |
| ES8311 / TCA9554 / FT3168 / PCF85063 / QMI8658 / AXP2101 地址 | 0x18 / 0x20 / 0x38 / 0x51 / 0x6B / 0x34 |

### 显示时序（sdkconfig.defaults 实际值）

| 配置 | 值 |
| --- | --- |
| `DISPLAY_PMU_PREPARE_DELAY_MS` | 100 |
| `DISPLAY_POWER_OFF_HOLD_MS` | 150 |
| `DISPLAY_POWER_ON_SETTLE_MS` | 350 |
| `DISPLAY_PANEL_RESET_SETTLE_MS` | 150 |
| `DISPLAY_PANEL_INIT_SETTLE_MS` | 200 |
| `DISPLAY_DISP_ON_SETTLE_MS` | 120 |
| `DISPLAY_POST_INIT_DELAY_MS` | 200 |
| `DISPLAY_BRIGHTNESS_SETTLE_DELAY_MS` | 120 |
| `DISPLAY_LOW_BRIGHTNESS_PERCENT` | 20 |
| `DISPLAY_DEFAULT_BRIGHTNESS_PERCENT` | 80 |
| `DISPLAY_SELF_TEST_DELAY_MS` | 250 |
| `DISPLAY_DELAY_I2C_SCAN` | y |

注：Kconfig 内的菜单默认值小于 sdkconfig.defaults 的部分值（如 power on settle 120ms），以 sdkconfig.defaults 为准；如需调整优先改本文件后重新构建。
