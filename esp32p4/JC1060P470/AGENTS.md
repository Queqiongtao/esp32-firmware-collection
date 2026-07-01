# AI 工作指南

## 项目定位

这是一个基于 ESP-IDF 的 ESP32-P4 综合测试工程（JC1060P470），由 `mipi_dsi` 工程衍生而来。工程以 MIPI DSI 显示（JD9165 1024x600）+ GT911 触摸为基础，逐步集成 SD 卡、USB、摄像头、传感器、WiFi/蓝牙（需协处理器）、音频 I2S 等外设的综合测试能力。

显示链路参数与 `mipi_dsi` 工程完全一致，复用 `components/esp_lcd_jd9165/` 本地面板驱动。

## 关键模块

### 显示基础（已就绪）

- `main/JC1060P470_main.c`：应用入口，按顺序初始化 board、panel、LVGL 端口并加载 demo UI。
- `main/display_board.c`：MIPI DSI PHY 供电、背光 PWM/GPIO、刷新监视 GPIO。
- `main/display_panel.c`：MIPI DSI 总线、DPI 接口、JD9165 面板创建。
- `main/display_lvgl.c`：LVGL 任务、DIRECT 渲染模式双 fb buffer 分配、flush 回调（含整 fb `esp_cache_msync`）、LVGL 输入设备（indev）注册。渲染方案选型与问题根因详见 `docs/display_solution.md`。
- `main/display_touch.c`：I2C master 总线、GT911 触摸控制器初始化。
- `main/display_config.h`：集中定义分辨率、lane rate、DPI clock、framebuffer 数量、显示/触摸引脚、触摸缩放参数。
- `main/lvgl_demo_ui.c`：当前 LVGL 示例界面和动画负载。
- `components/esp_lcd_jd9165/`：本地 JD9165 面板驱动和初始化序列。
- `main/Kconfig.projbuild`：工程级显示选项，例如 DMA2D、刷新监视、背光诊断。
- `sdkconfig.defaults`、`sdkconfig.defaults.esp32p4`：当前工程默认配置。

### 外设测试模块（规划中，后续逐步实现）

每类外设独立成模块，遵循 `test_<外设>.c/.h` 命名约定，单一职责：

- SD 卡：`main/test_sdcard.c/.h`
- USB：`main/test_usb.c/.h`
- 摄像头：`main/test_camera.c/.h`
- 传感器：`main/test_sensor.c/.h`
- WiFi/蓝牙：`main/test_wireless.c/.h`（需协处理器芯片，如 ESP32-C6）
- 音频 I2S：`main/test_audio.c/.h`

外设公共参数（引脚、I2C 总线等）集中在 `main/test_config.h`，与 `display_config.h` 风格一致。主入口通过 Kconfig 选项控制各模块启用/禁用。

## AI 修改规则

- 默认使用中文与用户沟通，包括进度说明、最终总结、验证结果和代码评审结论；除非用户明确要求使用其他语言。
- Git 提交信息默认使用中文，除非用户明确要求使用其他语言。
- Git 提交信息前缀固定使用 `P4-当前文件夹名`。当前工程目录名是 `JC1060P470`，因此提交信息前缀固定为 `P4-JC1060P470: `。
- 修改前先读源码和配置，不要只依据 README 判断工程状态。
- 保持改动范围小，不做与当前目标无关的重构。
- 涉及帧率优化时，先区分 LVGL 渲染帧率与面板实际刷新率，不要混为一谈。
- 调整 `num_fbs`、`lane_bit_rate_mbps`、`dpi_clock_freq_mhz`、显示时序等参数时，一次只改一类变量，便于定位问题。
- 超过当前稳定参数的显示链路优化属于高风险试验，必须保留可回退路径。

## 代码规范

### 模块化与解耦

- 每个硬件子系统独立成模块（`display_board.c` 板级、`display_panel.c` 面板、`display_touch.c` 触摸、`display_lvgl.c` LVGL 适配），单一职责，不互相直穿内部状态。
- 模块对外只暴露 `display_<模块>_init()` 等 capability 接口，内部静态变量与寄存器配置不外泄。
- 触摸/输出设备的「硬件读取 + 坐标变换」归 `display_touch` 模块；LVGL 层只负责注册 indev 与回调，不直接引用控制器句柄类型、I2C API 或 native/panel 分辨率常量。
- 缩放、镜像、旋转等坐标变换参数集中在触摸模块内部，不散落到 LVGL 文件。
- 公共参数（引脚、分辨率、lane rate 等）集中在 `display_config.h`，源文件不出现裸数字或裸 GPIO 号。
- 外设测试模块遵循同样的解耦原则，对外暴露 `test_<外设>_init()` / `test_<外设>_run()` 等接口，不直穿其他模块内部状态。

### 资源生命周期与句柄持有

- LVGL 等需要跨函数引用的对象，用 file-static 句柄持有，禁止用 `lv_obj_get_child(parent, 负序号)` 反向索引依赖兄弟对象顺序。
- 新增 UI 控件前先确认是否会破坏既有“按子序号取对象”的隐式约定；如会，必须改用显式句柄。

### 第三方组件使用

- 引入或升级 managed component 前，先读其头文件和示例，确认必需的配置字段是否齐全（如 GT911 的 `driver_data`、`levels.reset/interrupt`），不允许用零初始化结构体蒙混。
- 不修改 `managed_components/` 下文件；问题必须在自研代码层绕过或加封装。

### 时序与阻塞

- 触摸/I2C 读、Flash/PSRAM 访问等阻塞操作不得放在 LVGL tick 任务高频路径；如必须放，先用 `ESP_LOGD` 替代 `ESP_LOGI` 避免日志阻塞。
- 初始化阶段需要的复位时序（RST 释放、INT 选址、上电延时）必须按芯片 datasheet 写完整，不依赖引脚浮空默认电平。

### Cache 一致性

- 涉及 PSRAM + DMA 的路径（CPU 写、DMA 读），必须由应用层显式调用 `esp_cache_msync(..., ESP_CACHE_MSYNC_FLAG_DIR_C2M)` 把 CPU 写入的数据写回 PSRAM，不依赖 LVGL 内部 cache 管理。
- LVGL 9.4.0 的 `lv_draw_buf_copy` 不做 cache write back（源码 0 处 `esp_cache_msync` 调用，`flush_cache_cb` 默认为 NULL）。DIRECT 模式下 `refr_sync_areas` 用 CPU memcpy 同步双 fb 的 dirty 区，若不在 flush_cb 中补 `esp_cache_msync`，DMA 会读到 PSRAM 旧数据导致残影。
- 详见 `docs/display_solution.md` 残影根因与修复章节。

### 风格一致性

- 函数命名、参数顺序、日志 TAG 命名与同模块既有代码保持一致。
- 同一文件内错误处理统一用 `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`，不得混用裸 `if (ret != ESP_OK) return`。
- 头文件使用 `#pragma once`，对外接口写在 `display_<模块>.h` / `test_<外设>.h`，内部类型写在 `.c` 或独立的 `*_private.h`。

### 改动管控

- 修复一个 bug 时，改动只覆盖该 bug 必要的最小集；不顺手做与当前目标无关的重构、格式化或改名。
- 重构与 bugfix 不混在同一 commit；先提交 bugfix（已验证），再启动重构。

## 硬件与显示注意事项

- 当前工程默认目标为 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`。
- 当前工程芯片版本为 ESP32-P4 **V1.0**（`CONFIG_ESP32P4_REV_MIN_FULL=100`，`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`），IDF 使用 `register/hw_ver1/` 寄存器定义。
- 当前 LVGL 渲染模式为 **DIRECT + 双 framebuffer ping-pong**，flush_cb 中做整 fb `esp_cache_msync(DIR_C2M)` 消除残影。
- 当前板级控制引脚按工程实际定义为：
  - 背光：`GPIO23`
  - LCD 复位：`GPIO27`
  - 触摸复位：`GPIO22`
  - 触摸中断：`GPIO21`
- 当前工程默认使用内部 `LDO channel 3` 提供 `2.5V` MIPI DPHY 供电。
- 已知较高 `dpi_clock_freq_mhz` 可能引入白条、闪屏或不稳定，验证时必须逐档测试。

## 验证要求

- 代码改动后优先执行构建验证。
- 涉及显示链路参数调整时，最终说明里要明确：
  - 当前 `num_fbs`
  - 当前 lane rate
  - 当前 DPI clock
  - 是否完成上板验证
  - 是否观察到白条、花屏、闪烁或黑屏
- 如果没有做硬件验证，必须明确说明结论仅来自静态修改或编译验证。
