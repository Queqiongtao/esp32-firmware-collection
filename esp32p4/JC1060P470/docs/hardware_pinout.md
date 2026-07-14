# JC1060P470 硬件引脚说明

本文档记录 JC1060P470 工程（ESP32-P4 综合测试工程）所有外设的硬件连接与 GPIO 分配。

## 概述

- **芯片**：ESP32-P4 V1.0（`CONFIG_ESP32P4_REV_MIN_FULL=100`）
- **板型**：FIB（与 ESP32-P4 Function EV Board 引脚兼容）
- **参考例子**：`JC1060P470/Demo_IDF/examples/`（基于 ESP-IDF 5.4-dev）
- **本工程 IDF**：v6.0.1
- **参考 BSP 头文件**：`usb_extend_screen/components/esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h`

> 引脚参数在源码中集中定义：显示/触摸见 `main/display_config.h`，外设测试见 `main/test_config.h`。源文件不出现裸 GPIO 号。

> 当前板上有两个独立的 USB Type-C 口，但能力不同：`FULL USB`（丝印）走 `GPIO24(DM)/GPIO25(DP)` → `USB_INT_PHY0`，即 USB Serial/JTAG（USJ）控制器，固定功能 CDC-ACM（固件下载 / IDF log），**不是 DWC OTG**，不能跑 TinyUSB 自定义 device 栈或 USB Host；`HIGH USB`（丝印）走专用封装脚 `49/50` → 内部 UTMI High-Speed PHY，即 DWC OTG controller[0]，480Mbps，可作 HS Device（TinyUSB）或 HS Host（usb_host + UVC），同一时刻只能选一种角色。USB 子系统方案详见 [docs/usb_solution.md](./usb_solution.md)。
>
> 本文档以下统一区分“GPIO 编号”和“封装物理脚号”，避免把 `GPIO52`（BAT ADC 采样）与任何 USB 封装脚号混淆。

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
- **与 Hosted 共存注意事项**：当前板上 `ESP32-C6` hosted WiFi 使用 `SDMMC Slot 1`；SD 卡使用 `Slot 0`。在 ESP-IDF `v6.0.x` 下，SD 卡路径需要复用 ESP-Hosted 已完成的 SDMMC host controller 初始化，避免重复初始化共享 SDMMC host controller。
- **当前软件状态**：`Slot 0` SD 卡与 `Slot 1` Hosted 已完成共存验证，当前工程已上板验证文件系统挂载与读写回环通过。

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
- **USB Device 卡片**：当前 `CDC-ACM` echo demo 使用 `HIGH USB` 口（封装脚 `49/50`，内部 UTMI HS PHY，对应 DWC OTG controller[0]）；`FULL USB` 口是 USJ 固定功能串口，不能跑自定义 device 栈。USB 端口划分与角色切换方案详见 [docs/usb_solution.md](./usb_solution.md)。
- **音频卡片**：播放/录音链路使用 ES8311 + 板载模拟 MIC + 扬声器功放，对应 I2S 引脚为 `GPIO9/10/11/12/13`，功放使能为 `GPIO20`。
- **电量卡片**：`GPIO52` 接在 BAT 分压点，分压关系为 `BAT -> 68K -> GPIO52 -> 100K -> GND`，软件通过 ADC2 通道读取分压点并换算为电池电压与估算百分比。
- **WiFi 卡片**：已接入 hosted WiFi 扫描与连接路径，基于 `ESP32-P4 <-> ESP32-C6` SDIO 链路工作。
- **BLE 卡片**：当前仅展示 hosted 状态，BLE 业务路径尚未接入。

该映射仅说明“当前页面上的功能块对应哪类硬件资源”，不替代具体 UI 文案或布局说明；页面结构和交互以 `main/lvgl_demo_ui.c` 为准。

### 1.5 无线协处理器连接（板级已连接，WiFi 已接入）

当前板子为 `ESP32-P4 + ESP32-C6` 组合，`ESP32-P4` 与 `ESP32-C6` 之间通过 `SDIO` 连接。当前工程已基于这组板级连接接入 hosted WiFi 测试路径；BLE hosted 仍未接入。

| GPIO | 信号 | 说明 |
|------|------|------|
| 14 | SDIO D0 | `ESP32-P4 -> ESP32-C6` SDIO 数据线 0 |
| 15 | SDIO D1 | `ESP32-P4 -> ESP32-C6` SDIO 数据线 1 |
| 16 | SDIO D2 | `ESP32-P4 -> ESP32-C6` SDIO 数据线 2 |
| 17 | SDIO D3 | `ESP32-P4 -> ESP32-C6` SDIO 数据线 3 |
| 18 | SDIO CLK | `ESP32-P4 -> ESP32-C6` SDIO 时钟 |
| 19 | SDIO CMD | `ESP32-P4 -> ESP32-C6` SDIO 命令线 |

- **当前软件状态**：Hosted transport 与 WiFi STA 路径已接入当前工程并完成上板验证；启动日志中仍可能出现协处理器版本/RPC 元信息告警。
- **当前限制**：BLE hosted 业务路径尚未接入，当前 BLE 卡片不代表 BLE 功能已完成。

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

### 3.2 USB 口（当前板级实物连接）

板上两个 USB Type-C 口能力不同，不能互换：

| 接口 | 板级连接 | 控制器 / 能力 |
|------|----------|--------------|
| `FULL USB`（丝印） | 信号 GPIO `GPIO24(DM)/GPIO25(DP)` | `USB_INT_PHY0` = USB Serial/JTAG（USJ）控制器。固定功能 CDC-ACM 串口，固件下载 / IDF log 走这个口。**不是 DWC OTG**，不能跑 TinyUSB 自定义 device 栈或 USB Host。 |
| `HIGH USB`（丝印） | 封装脚 `49(DM)/50(DP)` | 内部 UTMI High-Speed PHY → DWC OTG controller[0]，480Mbps。可作 HS Device（TinyUSB CDC/MSC/HID）或 HS Host（usb_host + UVC/MSC/HID），同一时刻只能选一种角色。 |

- 烧录和 IDF log 请连接 `FULL USB` 口（USJ），主机侧枚举为 USB 串口。
- 当前 `USB Device CDC-ACM` echo 测试请连接 `HIGH USB` 口（TinyUSB HS Device），主机侧枚举为另一个虚拟串口；此时该口不能同时跑 UVC Host。
- USB UVC 摄像头测试也使用 `HIGH USB` 口，但角色为 HS Host，与 CDC Device 互斥（运行时角色切换方案详见 [docs/usb_solution.md](./usb_solution.md)）。
- `FULL USB` 与 `HIGH USB` 在主机侧都可能是串口设备，但前者只用于烧录/日志，后者是应用层 CDC/Host 数据口，不要混用。
- 芯片底层控制器/PHY 能力仍由 ESP32-P4 USB 子系统决定，本工程板级文档按当前实物接口角色记录。

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
| 14 | ESP32-C6 SDIO D0 | 已用 |
| 15 | ESP32-C6 SDIO D1 | 已用 |
| 16 | ESP32-C6 SDIO D2 | 已用 |
| 17 | ESP32-C6 SDIO D3 | 已用 |
| 18 | ESP32-C6 SDIO CLK | 已用 |
| 19 | ESP32-C6 SDIO CMD | 已用 |
| 20 | 功放使能 | 已用 |
| 21 | 触摸 INT | 已用 |
| 22 | 触摸 RST | 已用 |
| 23 | 背光 PWM | 已用 |
| 24 | USB Serial/JTAG (USJ) DM | 板级专用 |
| 25 | USB Serial/JTAG (USJ) DP | 板级专用 |
| 27 | LCD RST | 已用 |
| 39 | SD D0 | 已用 |
| 40 | SD D1 | 已用 |
| 41 | SD D2 | 已用 |
| 42 | SD D3 | 已用 |
| 43 | SD CLK | 已用 |
| 44 | SD CMD | 已用 |
| 49 | High-Speed USB OTG DM（封装脚，UTMI HS PHY） | 板级专用 |
| 50 | High-Speed USB OTG DP（封装脚，UTMI HS PHY） | 板级专用 |
| 52 | BAT 分压采样输入 | 已用 |

> 未列出的 GPIO 在当前板级文档中未定义用途，不纳入当前硬件连接关系。

---

## 5. 引脚复用注意事项

以下 GPIO 在当前工程中存在复用，需要按现有初始化方式共享，不能重复初始化：

| GPIO | 复用关系 | 说明 |
|------|---------|------|
| 7 / 8 | 触摸 / RTC / 音频 Codec 共用 I2C 总线 | 应用层通过 `display_touch_get_i2c_bus()` 获取已初始化的 bus handle，不重复创建总线 |
| 14 / 15 / 16 / 17 / 18 / 19 | `ESP32-P4 <-> ESP32-C6` SDIO 板级连接 | 当前板上已接到 `ESP32-C6`，后续实现无线模块时不应再挪作它用 |
| 52 | BAT 采样输入，封装脚号 52 在当前工程中未作 USB 用途 | 当前工程中 `GPIO52` 仅表示 ADC 采样 GPIO；USB 信号不使用 GPIO52 |
