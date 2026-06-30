# AI 工作指南

## 仓库结构

- 根目录只是分类容器，不是构建根。每个子工程都是独立的 ESP-IDF 项目，必须在具体工程目录内运行命令。
- 实际目录（根 README 中的结构已过时）：
  - `esp32p4/hello_world`：最小示例，无 `dev.sh`，直接使用 `idf.py`。
  - `esp32p4/mipi_dsi`：JD9165 MIPI DSI 显示工程，提供 `dev.sh`（使用 `eim`）。
  - `esp32s3/blink`：最小示例，无 `dev.sh`，直接使用 `idf.py`。
  - `esp32s3/clock`：AMOLED 时钟工程，提供 `dev.sh`（使用 `eim`）。
- 子工程若存在自己的 `AGENTS.md`，其具体约束优先于本文件。

## 构建入口

- 有 `dev.sh` 的工程（`esp32s3/clock`、`esp32p4/mipi_dsi`）优先使用它，不要假设本机已手动 source ESP-IDF 的 `export.sh`：

  ```bash
  ./dev.sh build
  ./dev.sh -p /dev/ttyUSB0 flash-monitor
  ./dev.sh ports
  ```

- 无 `dev.sh` 的工程（`esp32s3/blink`、`esp32p4/hello_world`）使用标准 ESP-IDF 命令：

  ```bash
  idf.py set-target <esp32s3|esp32p4>
  idf.py build
  ```

## 通用规则

- 默认使用中文与用户沟通，除非用户明确要求使用其他语言。
- 修改前先查看源码、构建配置和当前工程目录结构，不要只依据 README 判断工程状态。
- Git 提交信息前缀固定使用 `平台缩写-当前工程文件夹名: `：
  - `esp32p4/<工程目录>` → `P4-<工程目录名>: `
  - `esp32s3/<工程目录>` → `S3-<工程目录名>: `
  - 示例：`P4-mipi_dsi: `、`S3-clock: `

## 注意事项

- `esp32s3/clock` 和 `esp32p4/mipi_dsi` 通过 `eim` 调用 ESP-IDF，`dev.sh` 内部会执行 `eim run "idf.py ..."`。
- 部分子工程目录下存在空的 `.git` 目录，它们不是独立仓库或子模块；本仓库以根目录 `.git` 为唯一仓库。
