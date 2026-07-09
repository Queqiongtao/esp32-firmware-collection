# USB Extend Screen Reference

本文记录 `amoled_subscreen` 后续实现 USB 副屏功能时的参考资料、实现映射和分阶段计划。

## 参考示例

- ESP-IoT-Solution USB Extend Screen:
  <https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen>

该示例目标是让 ESP32-S3 / ESP32-P4 / ESP32-S31 开发板作为 Windows secondary display 使用。它通过 Windows IDD 显示驱动把设备注册为扩展屏，再通过 USB Vendor 接口把图像帧传到设备端，最后由设备端绘制到 LCD。

示例 README 中值得关注的能力点：

- ESP32-S3-LCD-EV-BOARD 参考指标约为 `800x480@13FPS`
- 支持显示、触摸和音频
- 纯显示设备 PID 使用 `0x2987`
- 复合设备 PID 使用 `0x2986`
- 可配置分辨率、最大 FPS、JPEG 质量和最大帧大小
- Windows 驱动当前不支持竖屏

## 关键模块

建议优先参考示例中的这些模块和职责，而不是直接复用其 LCD 板级初始化：

- `app_vendor.c`
  - 初始化 TinyUSB Vendor device
  - 通过 Vendor RX callback 接收 PC 端数据
  - 解析 `udisp_frame_header_t`
  - 将完整帧放入 filled frame queue
  - `transfer_task` 取出帧并调用 LCD 绘制入口
- `usb_frame.c`
  - 管理 empty / filled frame queue
  - 管理多个 USB frame buffer
  - 在 ESP32-S3 上使用 PSRAM 分配接收缓存
  - 提供 frame allocate / add / send / get / return 等队列 API
- LCD draw 层
  - 示例面向 ESP 官方 LCD EV Board
  - 本工程不能直接复用该 LCD 初始化
  - 只参考“收到帧后统一进入 draw 函数”的接口边界
- Windows driver 文档
  - 用于确认 VID / PID、驱动安装、扩展屏枚举和配置限制

## 与本工程的映射

`amoled_subscreen` 当前已经具备 Waveshare AMOLED 1.8 的板级显示初始化，应保留这部分：

- `board_display_init()`
- `board_display_new()`
- `esp_lcd_panel_draw_bitmap()`
- SH8601 QSPI 面板驱动
- PMU / IO expander 上电和复位时序

后续 USB 副屏功能应新增独立模块，不要把 USB 收帧逻辑混入板级组件：

- 新增 USB 显示链路组件，例如 `components/usb_extend_screen/`
- 应用层启动时先初始化板级显示，再启动 USB Vendor device
- USB 收到完整帧后调用本工程 LCD 绘制适配函数
- 无 USB 连接或无帧时保留当前 placeholder 画面

推荐的本工程绘制入口：

```c
esp_err_t subscreen_lcd_draw_rgb565_rect(int x, int y, int width, int height, const void *pixels);
esp_err_t subscreen_lcd_draw_frame(const void *frame, size_t len, int width, int height, int format);
```

首版可以只实现 RGB565 路径；JPEG 解码和旋转优化放到后续阶段。

## 风险与约束

- 本屏物理分辨率是 `368x448`，属于竖屏比例。
- 官方示例 Windows 驱动提示当前不支持竖屏。
- 首版建议对外声明 `448x368` 横屏分辨率，再在设备端旋转或裁剪绘制到物理屏。
- 如果先验证链路，也可以临时声明较小横屏分辨率，例如 `320x240`，确认 USB 收帧和 LCD 绘制稳定后再调到目标尺寸。
- USB 副屏首版只做显示；触摸 HID 和 UAC 音频会显著增加描述符、驱动和调试复杂度。
- ESP32-S3 帧率会受 USB 带宽、PSRAM 拷贝、JPEG 解码和 QSPI LCD 刷新共同限制，不能直接假设达到 PC 显示器体验。

## 分阶段实现计划

### Phase 1: USB Vendor 连接骨架

- 引入 TinyUSB device / Vendor 相关依赖。
- 参考 `usb_extend_screen` 建立 USB descriptor、VID / PID 和 Vendor endpoint。
- 先禁用触摸和音频，仅保留显示接口。
- 实现基本控制命令日志，例如 connect、disconnect、frame header received。
- 构建验证通过后，用 Windows 驱动确认设备可枚举为扩展屏候选设备。

### Phase 2: 帧接收与本地绘制

- 参考 `usb_frame.c` 建立 empty / filled frame queue。
- 在 PSRAM 中分配多个接收 buffer。
- 实现完整帧拼包、frame id 校验和错误丢帧。
- 新增 `subscreen_lcd_draw_*` 适配层，将 RGB565 frame 写入 SH8601。
- USB 未收到有效帧时保留 placeholder；收到首帧后切换到副屏画面。

### Phase 3: 分辨率、旋转和性能

- 首先验证 `320x240` 或 `448x368` 横屏模式。
- 若 Windows 驱动不接受 `368x448`，设备端实现 `448x368 -> 368x448` 旋转绘制。
- 增加帧率限制，避免 USB 接收任务压垮 LCD 刷新任务。
- 根据实际瓶颈评估 RGB565、JPEG 或局部更新方案。

### Phase 4: 交互扩展

- 在纯显示稳定后再考虑触摸 HID。
- 触摸坐标需要按实际旋转方向做坐标变换。
- 音频 UAC 默认不做，除非明确需要“显示器音频设备”体验。
- 后续可以增加亮度控制、断连回退画面和设备状态页。

## 首版默认取舍

- 只做 Windows USB 副屏显示。
- 使用 USB Vendor 接口，不使用 CDC 串口作为主显示链路。
- 优先复用官方示例的 USB 协议和 frame queue 思路。
- 保留本工程现有 AMOLED 板级初始化。
- 不直接移植 ESP32-S3-LCD-EV-BOARD 的 LCD 代码。
- 默认禁用触摸和音频。
