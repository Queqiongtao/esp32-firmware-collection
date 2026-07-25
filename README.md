# ESP32 固件集合

本仓库同时保存按芯片分类的独立 ESP-IDF 工程，以及包含多个平台端点的复合工程。仓库根目录不是统一构建根，请进入具体构建根执行对应命令。

## 目录结构

```text
esp32-firmware-collection/
├── companion_display/         Companion Display v2 复合工程
│   ├── desktop_client/        Tauri 2 + Vue 3 PC 客户端
│   ├── firmware/
│   │   └── esp32s3/           S3 AMOLED Wi-Fi 设备固件
│   ├── docs/
│   └── PROTOCOL.md
├── esp32s3/
│   ├── clock/
│   ├── amoled_subscreen/
│   ├── waveshare_amoled_1_8/
│   └── blink/
├── esp32p4/
│   ├── JC1060P470/
│   ├── mipi_dsi/
│   └── hello_world/
└── wireless_display/          独立的 WDP/WebRTC 协议设计
```

`companion_display` 延续现有 WebSocket + JPEG + UDP 的 Companion Display v2，将公共协议、PC 客户端和设备固件放在同一产品目录。未来 ESP32-P4 固件放入 `companion_display/firmware/esp32p4`，与当前 S3 固件保持独立构建。`wireless_display` 是不兼容的 WDP/WebRTC 新协议，两者不共享版本号或实现。

## 构建

带 `dev.sh` 的固件工程优先使用：

```bash
cd companion_display/firmware/esp32s3
./dev.sh build
```

无 `dev.sh` 的示例使用当前 ESP-IDF 环境中的 `idf.py`。具体烧录、端口和依赖说明以各工程 README 为准。

Companion Display PC 客户端使用 Node.js、Rust stable 和 Tauri 系统依赖：

```bash
cd companion_display/desktop_client
npm ci
npm run tauri:dev
```

协议、适配要求和验证命令见 [Companion Display](companion_display/README.md)。

## 新增工程

- 独立固件项目放入对应芯片目录，并拥有独立 `CMakeLists.txt`、配置和构建说明。
- 同一产品的协议、客户端和多平台固件放入顶层产品目录；设备固件按 `firmware/<芯片>/` 组织并保持独立构建。
- 跨平台 WDP/WebRTC 工作继续在 `wireless_display/` 中按其协议评审流程推进。
