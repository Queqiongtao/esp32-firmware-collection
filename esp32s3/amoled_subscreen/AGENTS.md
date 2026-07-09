# AI 工作指南

## 项目定位

这是一个面向 Waveshare `ESP32-S3-Touch-AMOLED-1.8` 开发板的小副屏工程骨架。

当前阶段目标是建立一个可构建、可点亮屏幕的独立小副屏应用骨架。首版只做显示初始化和占位画面，不移植完整官方 Demo，也不复用 `esp32s3/clock` 的时钟应用层。

## 依赖与参考

- 不依赖 GitHub 上的官方 Demo 代码，也不依赖 Waveshare 组件注册表里的 BSP
- 仅依赖 ESP-IDF 官方组件、驱动和本工程内本地组件
- `espressif/*` 命名空间组件视为官方可用；允许引入 `waveshare/*` 等第三方组件，但应优先封装在本地模块内，避免应用层直接耦合第三方 BSP API
- 不要复用 `esp32s3/clock/components/esp32_s3_touch_amoled_1_75` 作为本工程 BSP；1.75 寸和 1.8 寸板卡硬件不一致
- 可以参考 `esp32s3/clock/dev.sh` 的工程管理方式，但不要直接带入其 RTC、IMU、主题和设置菜单逻辑

## 常用命令

使用本工程目录内脚本：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
```

## AI 修改规则

- 修改前先读本工程源码、`sdkconfig.defaults` 和本地 `components/`
- 优先保持工程骨架清晰，避免在功能未落地前堆积无效抽象
- 各个功能实现必须保持模块化、工程化、正规化；优先拆分为板级驱动、副屏业务、应用入口三个层次，避免把硬件访问、状态机、日志和显示逻辑长期混写在 `main` 中
- 新增板级能力时优先落在本工程 `components/board_amoled_1_8/`，不要重新引入外部 BSP 依赖
- 代码改动后至少执行：

```bash
./dev.sh build
```

- 默认使用中文沟通
- Git 提交信息前缀固定使用 `S3-amoled_subscreen: `
