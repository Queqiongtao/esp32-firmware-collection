# AI 工作指南

## 项目定位

这是一个基于 ESP-IDF 的 ESP32-P4 MIPI DSI 显示示例工程，当前工程已针对 1024x600 的 JD9165 屏进行适配。

当前目标不是保留官方通用示例状态，而是围绕本工程现有硬件连接、显示参数和实际运行结果继续优化显示效果与帧率。

## 关键模块

- `main/mipi_dsi_lcd_example_main.c`：应用入口，按顺序初始化 board、panel、LVGL 端口并加载 demo UI。
- `main/display_board.c`：MIPI DSI PHY 供电、背光 PWM/GPIO、刷新监视 GPIO。
- `main/display_panel.c`：MIPI DSI 总线、DPI 接口、JD9165 面板创建。
- `main/display_lvgl.c`：LVGL 任务、draw buffer、flush 回调。
- `main/display_config.h`：集中定义分辨率、lane rate、DPI clock、framebuffer 数量、引脚等显示参数。
- `main/lvgl_demo_ui.c`：当前 LVGL 示例界面和动画负载。
- `components/esp_lcd_jd9165/`：本地 JD9165 面板驱动和初始化序列。
- `main/Kconfig.projbuild`：工程级显示选项，例如 DMA2D、刷新监视、背光诊断。
- `sdkconfig.defaults`、`sdkconfig.defaults.esp32p4`：当前工程默认配置。

## AI 修改规则

- 默认使用中文与用户沟通，包括进度说明、最终总结、验证结果和代码评审结论；除非用户明确要求使用其他语言。
- Git 提交信息默认使用中文，除非用户明确要求使用其他语言。
- Git 提交信息前缀固定使用 `P4-当前文件夹名`。当前工程目录名是 `mipi_dsi`，因此提交信息前缀固定为 `P4-mipi_dsi: `。
- 修改前先读源码和配置，不要只依据 README 判断工程状态。
- 保持改动范围小，不做与当前目标无关的重构。
- 涉及帧率优化时，先区分 LVGL 渲染帧率与面板实际刷新率，不要混为一谈。
- 调整 `num_fbs`、`lane_bit_rate_mbps`、`dpi_clock_freq_mhz`、显示时序等参数时，一次只改一类变量，便于定位问题。
- 超过当前稳定参数的显示链路优化属于高风险试验，必须保留可回退路径。

## 硬件与显示注意事项

- 当前工程默认目标为 `JD9165`、`1024x600`、`RGB565`、`2-lane DSI`。
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
