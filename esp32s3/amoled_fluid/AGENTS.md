# AI 工作指南

## 项目定位

这是一个面向 Waveshare `ESP32-S3-Touch-AMOLED-1.8` 开发板的 AMOLED 方块物理效果实验工程（IMU 驱动）。

硬件与本仓库 `esp32s3/waveshare_amoled_1_8` 完全一致：368x448 竖屏 AMOLED（SH8601，QSPI 接口）+ FT3168 电容触摸 + QMI8658 IMU。当前阶段为可构建、可点亮的方块物理效果工程：屏上分布 40~60 个彩色方块，摇动/倾斜设备时方块跟随重力与晃动冲量运动，参数与交互细节留待真机迭代调优，不要在功能未落地前堆积无效抽象。

## 依赖与参考

- `esp32s3/waveshare_amoled_1_8` 是本工程的硬件与板级参考，板级能力直接复用其 `components/board_amoled_1_8/`
- 不移植官方 Demo 的完整应用层，也不复用 `esp32s3/clock` 的 RTC、IMU、主题和设置菜单逻辑
- `espressif/*` 命名空间组件视为官方可用；`waveshare/*` 等第三方组件允许引入，但必须封装在本地组件内，避免应用层直接耦合第三方 BSP API
- 不要复用 `esp32s3/clock/components/esp32_s3_touch_amoled_1_75` 作为本工程 BSP；1.75 寸和 1.8 寸板卡硬件不一致

## 代码分层

保持三个层次分离，避免把硬件访问、状态机、日志和显示逻辑长期混写在 `main` 中：

1. 板级驱动：`components/board_amoled_1_8/`，负责 I2C、EXIO、显示、触摸、背光等硬件能力
2. 渲染/效果层：效果组件（例如 `components/block_physics/`），只依赖板级显示接口与 IMU 数据，不直接访问硬件寄存器
3. 应用入口：`main/`，负责初始化顺序、任务编排与效果启动

## 常用命令

使用本工程目录内脚本（通过 `eim` 调用 ESP-IDF）：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
./dev.sh menuconfig
```

默认串口为 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=PORT` 环境变量覆盖；`eim` 日志默认写入 `/tmp/eim-amoled_fluid.log`，可用 `EIM_LOG_FILE` 覆盖。

## 文档维护

- 技术细节（显示链路、性能预算、触摸事件、物理方案）必须落实到 `doc/` 对应文档
- 修改行为或新增能力后，同步更新 `README.md` 与相关 `doc/` 文档
- 实验测量（帧率、延迟、功耗、踩坑）必须记录到 `doc/dev-log.md`

## AI 修改规则

- 修改前先读本工程源码、`sdkconfig.defaults` 和本地 `components/`
- 优先保持工程骨架清晰，避免在功能未落地前堆积无效抽象
- 新增板级能力时优先落在本工程 `components/board_amoled_1_8/`，不要重新引入外部 BSP 依赖
- 代码改动后至少执行：

```bash
./dev.sh build
```

- 默认使用中文沟通
- Git 提交信息前缀固定使用 `S3-amoled_fluid: `
