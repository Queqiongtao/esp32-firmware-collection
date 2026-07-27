# AI 工作指南

## 仓库结构

- 根目录只是工程容器，不是统一构建根。ESP-IDF、桌面客户端和协议工程必须在各自的构建根内运行命令。
- 实际工程目录：
  - `companion_display`：Companion Display v3 复合工程，包含公共协议、Tauri PC 客户端和按芯片划分的设备固件；ESP-IDF 构建根为 `companion_display/firmware/esp32s3` 与 `companion_display/firmware/esp32p4`。
  - `esp32p4/hello_world`：最小示例，无 `dev.sh`，直接使用 `idf.py`。
  - `esp32p4/mipi_dsi`：JD9165 MIPI DSI 显示工程，提供 `dev.sh`（使用 `eim`）。
  - `esp32p4/JC1060P470`：ESP32-P4 综合测试工程（显示+触摸+外设测试，由 `mipi_dsi` 衍生），提供 `dev.sh`（使用 `eim`）。
  - `esp32s3/blink`：最小示例，无 `dev.sh`，直接使用 `idf.py`。
  - `esp32s3/clock`：AMOLED 时钟工程，提供 `dev.sh`（使用 `eim`）。
  - `wireless_display`：硬件无关的 WebRTC 投屏协议与跨平台参考核心工程；当前处于文档设计阶段，不是 ESP-IDF 项目。
- 根目录 `README.md` 维护当前工程与公共目录导航；修改顶层结构时必须同步更新。
- 部分子工程目录下存在空的 `.git` 目录，它们不是独立仓库或子模块；本仓库以根目录 `.git` 为唯一仓库。
- 子工程若存在自己的 `AGENTS.md`，其具体约束优先于本文件。

## 构建入口

- 有 `dev.sh` 的工程优先使用它，不要假设本机已手动 source ESP-IDF 的 `export.sh`：

  ```bash
  ./dev.sh build
  ./dev.sh menuconfig
  ./dev.sh -p /dev/ttyUSB0 flash-monitor
  ./dev.sh ports
  ```

- `dev.sh` 通过 `eim` 调用 ESP-IDF。各 `dev.sh` 的端口传法不同：
  - `esp32p4/mipi_dsi`：无默认端口，使用 idf.py 风格传参：`./dev.sh flash -p /dev/ttyUSB0`；`eim` 日志默认写入 `/tmp/eim-mipi_dsi.log`，可用 `EIM_LOG_FILE` 覆盖。
  - `esp32p4/JC1060P470`：无默认端口，使用 idf.py 风格传参：`./dev.sh flash -p /dev/ttyUSB0`；`eim` 日志默认写入 `/tmp/eim-JC1060P470.log`，可用 `EIM_LOG_FILE` 覆盖。
  - `esp32s3/clock`：默认端口 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=` 环境变量覆盖。
  - `companion_display/firmware/esp32s3`：默认端口 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=` 环境变量覆盖。
  - `companion_display/firmware/esp32p4`：默认端口 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=` 环境变量覆盖；日志默认写入 `/tmp/eim-companion_display_p4.log`。
- 无 `dev.sh` 的工程使用标准 ESP-IDF 命令：

  ```bash
  idf.py set-target <esp32s3|esp32p4>
  idf.py build
  ```
- `companion_display/desktop_client` 使用独立的 Node.js/Rust 构建入口：`npm run build`、`npm run tauri:build`，Rust 测试在 `src-tauri` 中执行 `cargo test`。

## 通用规则

- 默认使用中文与用户沟通，包括进度说明、最终总结、验证结果和代码评审结论；除非用户明确要求使用其他语言。
- Git 提交信息默认使用中文，除非用户明确要求使用其他语言。
- Git 提交信息前缀固定使用 `平台缩写-当前工程文件夹名: `：
  - `esp32p4/<工程目录>` → `P4-<工程目录名>: `
  - `esp32s3/<工程目录>` → `S3-<工程目录名>: `
  - `companion_display` 的共享协议、桌面端或跨端修改 → `companion_display: `
  - `companion_display/firmware/esp32s3` 的固件修改 → `S3-companion_display: `
  - `companion_display/firmware/esp32p4` 的固件修改 → `P4-companion_display: `
  - 示例：`P4-mipi_dsi: `、`S3-clock: `、`companion_display: `
- 修改前先查看源码、构建配置和当前工程目录结构；修改顶层结构时同步更新根 `README.md`。
- 代码改动后优先执行构建验证：有 `dev.sh` 的工程用 `./dev.sh build`，无 `dev.sh` 的工程用 `idf.py build`。
