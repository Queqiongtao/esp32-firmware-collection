# 技术框架与依赖

## 固定版本

| 层 | 组件 | 约束/解析版本 |
| --- | --- | --- |
| SDK | ESP-IDF | `==6.0.1` |
| 语言 | C / C++ | GNU C 默认方言 / C++17 |
| UI | LVGL | `9.5.0` |
| 显示适配 | esp_lvgl_adapter | 清单 `^0.6.2`，锁文件当前 `0.6.3` |
| 网络协处理 | esp_hosted | `==2.12.8` |
| 远程 Wi-Fi | esp_wifi_remote | 清单 `<2.0`，锁文件当前 `1.6.0` |
| 触摸 | esp_lcd_touch_gt911 | `^1.2.0~2` |
| 音频 | esp_codec_dev | `~1.5` |
| 存储 | joltwallet/littlefs | `^1.20.1` |
| 数据 | cJSON / esp-mqtt | `^1.7.19` / `^1.0.0` |

最终可复现版本以 `dependencies.lock` 为准。依赖升级必须单独提交，并重新执行构建、启动、网络、显示和 OTA 回归。

## 构建约束

- 顶层 CMake 固定 `IDF_TARGET=esp32p4` 和项目版本 `v1.6.2-p4.1`。
- 当前主机 CMake 为 3.22.1，因此最低版本设为 3.22。
- IDF 6.0.1 私有 `periph_ctrl.h` 缺少 `stdbool.h`，仅为 `esp_driver_spi` 目标强制 include，禁止扩大到全局。
- IDF 内部代码依赖 GNU 扩展，禁止全局强制严格 C17。
- 应用保持 C++17；不可为单个 API 将全工程提升到 C++20。

## 图形与内存

显示使用 RGB565、MIPI DSI 双 framebuffer 和 `DOUBLE_DIRECT` tear avoidance。Hex PSRAM 200 MHz 保存帧缓冲和大块图片数据，内部 RAM 优先保留给 DMA、任务栈和网络控制结构。编译配置启用 adapter PPA 加速和 ESP LCD DMA2D copy hook。

当前 Hosted ready 检查需要 2.12.8 的私有 `is_transport_tx_ready()` 符号来覆盖事件注册前 transport 已连接的情况，因此不得放宽版本范围。移除该私有 ABI 前必须先验证公共事件接口能够覆盖冷启动、软重启和 C6 已运行三种时序。

## 版本策略

该工程是固定快照，不使用 Git 子模块追踪上游。合入上游修复时必须记录原提交、受影响文件、冲突决策和 P4 回归结果；许可证、DISCLAIMER 和版本标记必须保留。
