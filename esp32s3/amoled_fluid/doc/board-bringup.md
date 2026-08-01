# 板级启动时序

最后更新：2026-08-01

本文描述 `amoled_fluid` 从复位到可交互的完整启动顺序，映射到 `components/board_amoled_1_8/` 的公开接口。修改板级初始化时序时，以此文档与 Kconfig 为准。

## 启动总览

```text
app_main()
  1. 芯片与 Flash 信息打印
  2. board_display_init()          -> 显示链路完整初始化（见下）
  3. run_display_self_test()       -> 白/红/绿/蓝 + 彩条自检
  4. board_i2c_scan()              -> I2C 总线扫描（延迟到显示自检后）
  5. board_touch_new()             -> 触摸驱动创建（含复位脉冲）
  6. 主循环: board_touch_read() 10ms 轮询
```

## 显示初始化细节（`board_display_init` -> `board_display_module_get`）

| 步骤 | 实现 | 关键参数 |
| --- | --- | --- |
| 1. I2C 总线 | `board_i2c_init()` | port 0，SDA 15，SCL 14，400 kHz，内部上拉 |
| 2. LCD 电源使能 | `board_lcd_power_set(true)` | 经 TCA9554 EXIO1，active high |
| 3. PMU 准备 | `board_pmu_init()` + `board_pmu_display_power_cycle()` | AXP2101 @ 0x34；BLDO1=3.3V 先关后开；关闭保持 150ms，上电稳定 350ms（sdkconfig 值） |
| 4. LCD 复位脉冲 | `board_lcd_reset_pulse()` | 经 TCA9554 EXIO0，active low，脉冲 20ms |
| 5. SPI 总线 | `spi_bus_initialize(SPI2_HOST, ...)` | SCLK=11，D0-D3=4/5/6/7，QSPI quad 模式，DMA auto，最大传输 329,728 B（整帧） |
| 6. Panel IO | `esp_lcd_new_panel_io_spi` | CS=12，pclk 40 MHz，spi_mode 0，32-bit 命令 + 8-bit 参数，quad，队列深度 10 |
| 7. 面板创建 | `esp_lcd_new_panel_sh8601` | RGB565、RGB 元素序、QSPI 接口标志、初始化命令表 |
| 8. 面板复位 | `esp_lcd_panel_reset` | 复位稳定 150ms |
| 9. 面板初始化 | `esp_lcd_panel_init` | 初始化稳定 200ms |
| 10. 显示开启 | `esp_lcd_panel_disp_on_off(true)` | 稳定 120ms |
| 11. 背光低档 | `board_display_set_brightness_local(20%)` | SH8601 0x51 命令；稳定 120ms 后进入自检 |
| 12. 背光默认 | `board_display_module_init` 末尾 | 抬升至默认 80% |

全部延迟可经 `CONFIG_BOARD_AMOLED_1_8_DISPLAY_*` 配置（sdkconfig.defaults 中的实际值见 [dependencies-build.md](dependencies-build.md)）。

## SH8601 初始化命令表

`board_display.cpp` 内 `s_sh8601_init_cmds`：

| 命令 | 参数 | 说明 |
| --- | --- | --- |
| 0x11 | - | Sleep Out（120ms） |
| 0x44 | 01 D1 | Tear Scanline |
| 0x35 | - | Tear On（启用 TE 引脚） |
| 0x53 | 20 | 显示控制（部分） |
| 0x2A | 00 00 01 6F | 列地址 0..367（368 像素） |
| 0x2B | 00 00 01 BF | 页地址 0..447（448 像素） |
| 0x51 | 00 | 亮度 0（10ms） |
| 0x29 | - | Display On（10ms） |
| 0x51 | FF | 亮度 255（最大） |

注意：TE 引脚（GPIO13）当前仅保留日志，尚未接入绘制同步；后续流体渲染若要防撕裂，可基于 TE 做帧同步（见 [display-pipeline.md](display-pipeline.md)）。

## 触摸初始化细节

`board_touch_new()` 首次调用时：

1. 初始化 I2C（已就绪则跳过）
2. 尝试 PMU/IO 扩展器预初始化（失败仅告警）
3. 经 TCA9554 EXIO2 对 FT3168 复位：active low 脉冲 20ms + 稳定 120ms
4. 探测候选地址（0x38/0x14/0x15/0x5D）并打印
5. 固定使用 0x38 创建驱动（`esp_lcd_touch_new_i2c_ft5x06` 兼容路径）
6. 运行时配置：active mode、禁止 monitor timer、active period 12

触摸失败时带 500ms 退避重试，直到成功（详见 [touch-input.md](touch-input.md)）。

## 外设接口（当前未在 main 中使用）

板级组件保留以下能力，供后续功能选择启用：

| 接口 | 芯片 | 地址/引脚 | 说明 |
| --- | --- | --- | --- |
| `board_pmu_get_status` | AXP2101 | 0x34 | 电池电压/百分比/充电状态 |
| `board_sdcard_mount/unmount` | SDMMC 1-bit | D0=3 CMD=1 CLK=2 | 挂载到 `/sdcard`，失败不格式化 |
| `board_rtc_*` | PCF85063 | 0x51 | 读写时间 |
| `board_imu_*` | QMI8658 | 0x6B | 加速度/陀螺仪（±2g/±2000dps @128Hz） |
| `board_audio_*` | ES8311 + I2S | 0x18 | 16 kHz 双声道音频编解码 |
