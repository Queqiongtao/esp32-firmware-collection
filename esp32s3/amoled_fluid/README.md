# amoled_fluid

面向 Waveshare `ESP32-S3-Touch-AMOLED-1.8` 开发板的 AMOLED 方块物理效果实验工程：屏幕上分布 40~60 个混合大小彩色方块，摇动/倾斜设备（板载 QMI8658 IMU）时方块跟随重力与晃动冲量运动、互相碰撞堆叠，模拟物理引擎效果。

## Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8 V1
- Chip: ESP32-S3
- Flash: 16 MB（QIO）
- PSRAM: 启用，octal 80 MHz，支持取指与只读数据（XIP）
- CPU: 240 MHz，性能优化编译
- Display controller: SH8601（368x448 竖屏，QSPI，RGB565）
- Touch controller: FT3168
- 默认串口: `/dev/ttyACM0`，控制台波特率 2,000,000

## Reference

- 硬件与板级参考：`esp32s3/waveshare_amoled_1_8`
- 官方 Demo 仓库（仅作硬件参考，不移植代码）：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8>

## Current Status

当前为可构建、可点亮的第一版方块物理效果工程：

- `dev.sh` 构建/烧录/监控/清理/menuconfig 辅助脚本
- 本地 `components/board_amoled_1_8/` 板级组件：I2C、TCA9554 EXIO、SH8601 显示初始化与背光、FT3168 触摸
- 新增 `components/block_physics/` 物理组件：自研定点（Q8.8/Q16.16）AABB 方块物理（算法参考 Box2D Lite，输入处理参考 Android AccelerometerPlay），60 Hz 物理 + 30 FPS 渲染双核任务、PSRAM 三缓冲全帧提交
- IMU 交互：倾斜 -> 方块沿重力方向滑动；晃动 -> 随机冲量飞散；静置 -> 缓慢稳定堆叠；IMU 失联回退默认重力
- 板级外设接口（PMU、SD、RTC、IMU、音频）随参考工程保留，未在本工程验证
- 启动流程：板级初始化 -> 方块物理创建 -> IMU 采样任务（100 Hz）-> 物理/渲染双核启动

待办：

- 真机标定 IMU 轴映射与符号（`CONFIG_BLOCK_PHYSICS_IMU_SWAP_XY/INVERT_X/INVERT_Y`）
- 真机调参（重力、阻尼、恢复系数、晃动阈值）并测量帧率、CPU/PSRAM 占用，记录到 [doc/dev-log.md](doc/dev-log.md)
- 后续迭代：TE 同步、脏区局部刷新、方块旋转/复杂形状

## Default Board Configuration

关键默认配置见 [doc/dependencies-build.md](doc/dependencies-build.md)，完整 pinout 见 [doc/hardware-pinout.md](doc/hardware-pinout.md)：

- I2C port: `0`，SDA `15`，SCL `14`，400 kHz
- LCD: CS `12`、PCLK `11`、D0-D3 `4/5/6/7`、TE `13`；分辨率 `368x448`；SPI host `2`
- LCD 电源/复位经 TCA9554 EXIO1/EXIO0 控制
- 背光通过 SH8601 亮度命令（0x51）设置，无独立背光 GPIO；启动阶段先低亮度 20%，再抬升至默认 80%
- Touch: INT `21`，复位经 TCA9554 EXIO2

## Commands

在本工程目录内运行：

```bash
./dev.sh build
./dev.sh ports
./dev.sh flash-monitor
./dev.sh -p /dev/ttyUSB0 flash-monitor
./dev.sh menuconfig
```

首次构建会通过 ESP-IDF 组件管理器从注册表拉取依赖（`waveshare/esp_lcd_sh8601`、`espressif/*` 等），需要网络；依赖清单见 [doc/dependencies-build.md](doc/dependencies-build.md)。

## Key Files

- `main/main.cpp`: 应用入口，板级初始化 + 方块物理编排 + IMU 采样任务
- `components/board_amoled_1_8/`: 板级支持组件（与参考工程同源）
- `components/block_physics/`: 方块物理与渲染组件（AABB 求解、IMU 重力/晃动、双核任务）
- `sdkconfig.defaults`: 目标芯片、Flash/PSRAM 与板级 pinout 配置
- `doc/`: 技术文档索引见 [doc/README.md](doc/README.md)
