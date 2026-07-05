# JC1060P470 硬件引脚说明

本文档记录 JC1060P470 工程（ESP32-P4 综合测试工程）所有外设的硬件连接与 GPIO 分配。

## 概述

- **芯片**：ESP32-P4 V1.0（`CONFIG_ESP32P4_REV_MIN_FULL=100`）
- **板型**：FIB（与 ESP32-P4 Function EV Board 引脚兼容）
- **参考例子**：`JC1060P470/Demo_IDF/examples/`（基于 ESP-IDF 5.4-dev）
- **本工程 IDF**：v6.0.1
- **参考 BSP 头文件**：`usb_extend_screen/components/esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h`

> 引脚参数在源码中集中定义：显示/触摸见 `main/display_config.h`，外设测试见 `main/test_config.h`。源文件不出现裸 GPIO 号。

> 当前板上有两个独立的 USB Device 口：`Full-Speed USB Device` 连接专用 USB 封装脚 `49/50`；`High-Speed USB Device` 的信号 GPIO 为 `GPIO24/25`，对应芯片封装脚 `52/53`。本文档以下统一区分“GPIO 编号”和“封装物理脚号”，避免把 `GPIO52/53` 误写成 HS USB GPIO。

---

## 1. 已实现外设引脚（已上板验证）

### 1.1 显示、触摸与 RTC（`display_config.h` / `test_config.h`）

| GPIO | 信号 | 说明 |
|------|------|------|
| 7 | I2C SDA | 触摸与 RTC 共享 I2C 总线数据线（`I2C_NUM_1`, 400kHz） |
| 8 | I2C SCL | 触摸与 RTC 共享 I2C 总线时钟线 |
| 21 | Touch INT | GT911 触摸中断 |
| 22 | Touch RST | GT911 触摸复位 |
| 23 | 背光 PWM | LCD 背光（LEDC CH0, 20kHz） |
| 27 | LCD RST | JD9165 面板复位 |

- **RTC**：板载 `RX8025T-UC`，挂在触摸共享 I2C 总线（`GPIO7/8`），7-bit I2C 地址 `0x32`。启用 `CONFIG_EXAMPLE_ENABLE_TEST_RTC` 后，启动阶段读取时间/状态寄存器；若时间非法则写入固件构建时间，随后按 `TEST_RTC_READ_INTERVAL_MS`（默认 5000 ms）周期读取并打印时间。
- **I2C 复用原则**：RTC、触摸和音频 Codec 控制口复用同一条 I2C 总线，应用层通过 `display_touch_get_i2c_bus()` 获取已初始化的 bus handle，不重复初始化 `GPIO7/8`。

### 1.2 SD 卡（`test_config.h`）

| GPIO | 信号 | 说明 |
|------|------|------|
| 39 | D0 | SDMMC SLOT_0 数据线 0 |
| 40 | D1 | SDMMC SLOT_0 数据线 1 |
| 41 | D2 | SDMMC SLOT_0 数据线 2 |
| 42 | D3 | SDMMC SLOT_0 数据线 3 |
| 43 | CLK | SDMMC SLOT_0 时钟 |
| 44 | CMD | SDMMC SLOT_0 命令线 |

- **Slot**：SLOT_0（IO MUX 专用 UHS-I 引脚，直连 SDMMC 控制器，不经 GPIO 矩阵）
- **总线宽度**：4-bit
- **频率**：40MHz（`SDMMC_FREQ_HIGHSPEED`）
- **电源**：内部 LDO channel 4（`LDO_VO4`, 3.3V）
- **上拉**：依赖板载外部 10k 上拉，不启用内部上拉

### 1.3 音频 I2S（`test_config.h` / `test_audio.c`）

| GPIO | 信号 | 说明 |
|------|------|------|
| 9 | I2S DOUT | ESP32-P4 数据输出 → ES8311 DAC |
| 10 | I2S LCLK / WS | 字选择（左右声道） |
| 11 | I2S DSIN | ES8311 ADC 数据输入 → ESP32-P4 |
| 12 | I2S SCLK / BCLK | 位时钟 |
| 13 | I2S MCLK | 主时钟 |
| 20 | POWER_AMP | 功放使能 |

- **Codec**：`ES8311`，控制口复用触摸 I2C 总线（`I2C_NUM_1`, `GPIO7/8`），应用层通过 `display_touch_get_i2c_bus()` 获取 bus handle，不重复初始化总线。
- **MIC**：板载模拟麦克风 `MSM381A3729H9CP`，接入 ES8311 模拟 MIC 输入路径，当前软件按模拟 MIC（非 DMIC）方式配置。
- **I2S**：当前工程使用 `I2S0`，16-bit、stereo、16kHz，TX/RX 同时创建，硬件拓扑支持 ES8311 ADC + DAC 同时工作。
- **MCLK**：当前软件配置 `mclk_multiple = 384`。
- **功放**：`GPIO20` 仅用于扬声器功放使能，不参与录音链路。

### 1.4 当前综合 Demo 与硬件映射关系

当前 `example_lvgl_demo_ui()` 综合页面中，已落地的硬件映射关系如下：

- **显示与触摸卡片**：显示链路使用 JD9165 MIPI DSI 屏幕；触摸输入来自 GT911（`GPIO21/22` + `GPIO7/8` I2C）。
- **RTC 卡片**：数据来自板载 `RX8025T-UC` RTC（复用 `GPIO7/8` I2C）。
- **USB Device 卡片**：当前 `CDC-ACM` echo demo 使用板上的 `High-Speed USB Device` 口（信号 GPIO `GPIO24/25`，封装脚 `52/53`）。
- **音频卡片**：播放/录音链路使用 ES8311 + 板载模拟 MIC + 扬声器功放，对应 I2S 引脚为 `GPIO9/10/11/12/13`，功放使能为 `GPIO20`。
- **电量卡片**：`GPIO52` 接在 BAT 分压点，分压关系为 `BAT -> 68K -> GPIO52 -> 100K -> GND`，软件通过 ADC2 通道读取分压点并换算为电池电压与估算百分比。

该映射仅说明“当前页面上的功能块对应哪类硬件资源”，不替代具体 UI 文案或布局说明；页面结构和交互以 `main/lvgl_demo_ui.c` 为准。

---

## 2. LDO 通道分配

| LDO 通道 | 电压 | 用途 | 配置来源 |
|----------|------|------|---------|
| Channel 3 | 2.5V | MIPI DSI PHY 供电 | `DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN` |
| Channel 4 | 3.3V | SDMMC IO 供电 | `TEST_SDCARD_PWR_LDO_CHAN` |

---

## 3. 专用接口与板级专用连接

### 3.1 专用接口（非 GPIO）

| 接口 | 说明 |
|------|------|
| MIPI DSI | 2-lane，750 Mbps lane rate，54 MHz DPI clock → JD9165 1024x600 RGB565 LCD |
| MIPI CSI | 摄像头输入（`SOC_MIPI_CSI_SUPPORTED=1`），当前未用 |

### 3.2 USB Device 口（当前板级实物连接）

| 接口 | 板级连接 | 说明 |
|------|----------|------|
| Full-Speed USB Device | 封装脚 `49 (DM)` / `50 (DP)` | 板上独立的 Full-Speed USB Device 口。可用于芯片 USB Device 相关功能，但当前工程的 TinyUSB CDC-ACM demo 不使用这个口。 |
| High-Speed USB Device | 信号 GPIO `GPIO24/25`，对应封装脚 `52/53` | 板上独立的 High-Speed USB Device 口。当前工程的 TinyUSB CDC-ACM echo demo 默认使用这个口，主机侧会枚举为虚拟串口。 |

- 当前 `USB Device CDC-ACM` echo 测试请连接 `High-Speed USB Device` 口。
- `USB Serial/JTAG` 下载/日志口和 `USB Device CDC-ACM` 在主机侧都可能表现为串口设备，但二者功能不同；前者用于烧录/日志，后者是应用层 CDC 数据口。
- 芯片底层控制器/PHY 能力仍由 ESP32-P4 USB 子系统决定，但本工程板级文档只按当前实物接口角色记录为 `USB Device` 口。

### 3.3 电池分压采样

| GPIO | 信号 | 说明 |
|------|------|------|
| 52 | BAT_SENSE | 连接在 `68K` 与 `100K` 分压电阻中点，作为电池电压 ADC 采样输入 |

- 分压关系：`BAT -> 68K -> GPIO52(sample point) -> 100K -> GND`
- ADC 资源：`ADC_UNIT_2` / `ADC_CHANNEL_3`
- 软件换算：`Vbat = Vadc * (68K + 100K) / 100K = Vadc * 1.68`
- 当前页面第 5 张“电量”卡片显示 ADC 电压、电池电压与线性估算百分比，仅用于 demo 功能验证，不等价于精准电量计

---

## 4. GPIO 总览表（按编号排列）

| GPIO | 当前工程用途 | 状态 |
|------|-------------|------|
| 7 | 触摸 / RTC / 音频 Codec I2C SDA | 已用 |
| 8 | 触摸 / RTC / 音频 Codec I2C SCL | 已用 |
| 9 | I2S DOUT | 已用 |
| 10 | I2S LCLK / WS | 已用 |
| 11 | I2S DSIN | 已用 |
| 12 | I2S SCLK / BCLK | 已用 |
| 13 | I2S MCLK | 已用 |
| 20 | 功放使能 | 已用 |
| 21 | 触摸 INT | 已用 |
| 22 | 触摸 RST | 已用 |
| 23 | 背光 PWM | 已用 |
| 24 | High-Speed USB Device GPIO | 板级专用 |
| 25 | High-Speed USB Device GPIO | 板级专用 |
| 27 | LCD RST | 已用 |
| 39 | SD D0 | 已用 |
| 40 | SD D1 | 已用 |
| 41 | SD D2 | 已用 |
| 42 | SD D3 | 已用 |
| 43 | SD CLK | 已用 |
| 44 | SD CMD | 已用 |
| 49 | Full-Speed USB Device DM（封装脚） | 板级专用 |
| 50 | Full-Speed USB Device DP（封装脚） | 板级专用 |
| 52 | BAT 分压采样输入 | 已用 |

> 未列出的 GPIO 在当前板级文档中未定义用途，不纳入当前硬件连接关系。

---

## 5. 引脚复用注意事项

以下 GPIO 在当前工程中存在复用，需要按现有初始化方式共享，不能重复初始化：

| GPIO | 复用关系 | 说明 |
|------|---------|------|
| 7 / 8 | 触摸 / RTC / 音频 Codec 共用 I2C 总线 | 应用层通过 `display_touch_get_i2c_bus()` 获取已初始化的 bus handle，不重复创建总线 |
| 52 | BAT 采样输入，与 HS USB 封装脚号 52 容易混淆 | 当前工程中 `GPIO52` 仅表示 ADC 采样 GPIO，不表示 HS USB 信号 GPIO |
