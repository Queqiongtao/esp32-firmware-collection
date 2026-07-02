# JC1060P470 硬件引脚说明

本文档记录 JC1060P470 工程（ESP32-P4 综合测试工程）所有外设的硬件连接与 GPIO 分配。

## 概述

- **芯片**：ESP32-P4 V1.0（`CONFIG_ESP32P4_REV_MIN_FULL=100`）
- **板型**：FIB（与 ESP32-P4 Function EV Board 引脚兼容）
- **参考例子**：`JC1060P470/Demo_IDF/examples/`（基于 ESP-IDF 5.4-dev）
- **本工程 IDF**：v6.0.1
- **参考 BSP 头文件**：`usb_extend_screen/components/esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h`

> 引脚参数在源码中集中定义：显示/触摸见 `main/display_config.h`，外设测试见 `main/test_config.h`。源文件不出现裸 GPIO 号。

---

## 1. 已实现外设引脚（已上板验证）

### 1.1 显示与触摸（`display_config.h`）

| GPIO | 信号 | 说明 |
|------|------|------|
| 7 | I2C SDA | 触摸 I2C 总线数据线（`I2C_NUM_1`, 400kHz） |
| 8 | I2C SCL | 触摸 I2C 总线时钟线 |
| 21 | Touch INT | GT911 触摸中断 |
| 22 | Touch RST | GT911 触摸复位 |
| 23 | 背光 PWM | LCD 背光（LEDC CH0, 20kHz） |
| 27 | LCD RST | JD9165 面板复位 |

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

---

## 2. LDO 通道分配

| LDO 通道 | 电压 | 用途 | 配置来源 |
|----------|------|------|---------|
| Channel 3 | 2.5V | MIPI DSI PHY 供电 | `DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN` |
| Channel 4 | 3.3V | SDMMC IO 供电 | `TEST_SDCARD_PWR_LDO_CHAN` |

---

## 3. 专用接口（非 GPIO）

| 接口 | 说明 |
|------|------|
| MIPI DSI | 2-lane，750 Mbps lane rate，54 MHz DPI clock → JD9165 1024x600 RGB565 LCD |
| USB OTG | ESP32-P4 内置 USB OTG PHY（`SOC_USB_OTG_PERIPH_NUM=2`），专用引脚 |
| USB Serial JTAG | 烧录与串口日志输出（`USB-UART` 口） |
| MIPI CSI | 摄像头输入（`SOC_MIPI_CSI_SUPPORTED=1`），当前未用 |

---

## 4. 参考板未用外设引脚（FIB 板型，后续模块参考）

以下引脚由参考 BSP（FIB 板型）定义，本工程尚未实现对应模块，仅供后续 `test_<外设>` 模块开发参考。

### 4.1 音频 I2S（ES8311 Codec）

| GPIO | 信号 | 说明 |
|------|------|------|
| 9 | I2S DOUT | 数据输出 → ES8311 |
| 10 | I2S LCLK / WS | 字选择（左右声道） |
| 11 | I2S DSIN | 数据输入 ← ES8311 |
| 12 | I2S SCLK / BCLK | 位时钟 |
| 13 | I2S MCLK | 主时钟 |
| 20 | POWER_AMP | 功放使能（与 I3C SLV SCL 冲突） |

### 4.2 无线协处理器（SDIO，如 ESP32-C6）

| GPIO | 信号 | 说明 |
|------|------|------|
| 6 | WIRELESS_WKUP | 协处理器唤醒 |
| 14 | WIRELESS_D0 | SDIO 数据线 0 |
| 15 | WIRELESS_D1 | SDIO 数据线 1 |
| 16 | WIRELESS_D2 | SDIO 数据线 2 |
| 17 | WIRELESS_D3 | SDIO 数据线 3 |
| 18 | WIRELESS_CLK | SDIO 时钟 |
| 19 | WIRELESS_CMD | SDIO 命令线 |
| 23 | WIRELESS_EN | 协处理器使能（与背光冲突） |

### 4.3 I3C

| GPIO | 信号 | 说明 |
|------|------|------|
| 32 | I3C MST SCL | I3C 主机时钟 |
| 33 | I3C MST SDA | I3C 主机数据 |
| 20 | I3C SLV SCL | I3C 从机时钟（与功放冲突） |
| 21 | I3C SLV SDA | I3C 从机数据（与触摸中断冲突） |

### 4.4 Probe IO

| GPIO | 信号 | 说明 |
|------|------|------|
| 45 | PROBE_IO_45 | 调试探针 |
| 46 | PROBE_IO_46 | 调试探针 |
| 47 | PROBE_IO_47 | 调试探针 |
| 48 | PROBE_IO_48 | 调试探针 |

---

## 5. GPIO 总览表（按编号排列）

| GPIO | 当前工程用途 | 参考板备用用途 | 状态 |
|------|-------------|---------------|------|
| 6 | — | 无线 WKUP | 未用 |
| 7 | 触摸 I2C SDA | —（同） | 已用 |
| 8 | 触摸 I2C SCL | —（同） | 已用 |
| 9 | — | I2S DOUT | 未用 |
| 10 | — | I2S LCLK | 未用 |
| 11 | — | I2S DSIN | 未用 |
| 12 | — | I2S SCLK | 未用 |
| 13 | — | I2S MCLK | 未用 |
| 14 | — | 无线 D0 | 未用 |
| 15 | — | 无线 D1 | 未用 |
| 16 | — | 无线 D2 | 未用 |
| 17 | — | 无线 D3 | 未用 |
| 18 | — | 无线 CLK | 未用 |
| 19 | — | 无线 CMD | 未用 |
| 20 | — | 功放 / I3C SLV SCL | 未用 |
| 21 | 触摸 INT | I3C SLV SDA | 已用 / 冲突 |
| 22 | 触摸 RST | — | 已用 |
| 23 | 背光 PWM | 无线 EN | 已用 / 冲突 |
| 27 | LCD RST | — | 已用 |
| 32 | — | I3C MST SCL | 未用 |
| 33 | — | I3C MST SDA | 未用 |
| 39 | SD D0 | —（同） | 已用 |
| 40 | SD D1 | —（同） | 已用 |
| 41 | SD D2 | —（同） | 已用 |
| 42 | SD D3 | —（同） | 已用 |
| 43 | SD CLK | —（同） | 已用 |
| 44 | SD CMD | —（同） | 已用 |
| 45 | — | Probe IO | 未用 |
| 46 | — | Probe IO | 未用 |
| 47 | — | Probe IO | 未用 |
| 48 | — | Probe IO | 未用 |

> 未列出的 GPIO（0-5, 24-26, 28-31, 34-38, 49-54）在 FIB 板型中未定义用途，默认空闲。

---

## 6. 引脚冲突与复用注意事项

以下 GPIO 在不同外设间存在复用，**不能同时启用冲突的外设**：

| GPIO | 用途 A（当前工程） | 用途 B（参考板） | 说明 |
|------|-------------------|-----------------|------|
| 20 | — | 功放使能 / I3C SLV SCL | 功放与 I3C 从机不可共存 |
| 21 | 触摸中断 | I3C SLV SDA | I3C 从机与触摸不可共存，触摸是基础功能 |
| 23 | 背光 PWM | 无线使能 | 无线协处理器使能与背光复用，需确认板载设计 |
