# Companion Display

本目录统一保存 Companion Display v2 的公共协议、PC 客户端和设备固件。各端通过相同协议协作，但保留独立构建入口；公共客户端不得依赖某个设备工程的源码、构建产物或固定屏幕尺寸。

## 目录

- `PROTOCOL.md`：Companion Display v2 线上协议的权威定义。
- `docs/pc-client.md`：PC 客户端架构、数据流和排障说明。
- `desktop_client/`：Tauri 2 + Vue 3 桌面客户端。
- `firmware/esp32s3/`：当前 Waveshare ESP32-S3-Touch-AMOLED-1.8 设备固件。

WDP/WebRTC 位于仓库顶层 `wireless_display/`，它与本目录的 WebSocket/JPEG/UDP v2 协议相互独立，不共享版本号或兼容承诺。

## 设备适配要求

兼容设备必须实现 `PROTOCOL.md` 定义的 `/api/status`、WebSocket 控制通道、`CDP1` 完整帧，以及所声明的 TCP/UDP 视频通道。设备通过 `hello_ack.width`、`height` 和 `max_frame_bytes` 声明当前会话目标，客户端据此完成采集缩放、方向处理和 JPEG 限流。

当前 ESP32-S3 实现位于 `firmware/esp32s3`，默认协商 `368x448` 和约 100 KiB JPEG。后续 ESP32-P4 实现放入 `firmware/esp32p4`，可以协商不同尺寸和帧上限，无需复制或修改 PC 客户端；P4 工程在实际适配开始时创建，本次不放置空目录。

## PC 客户端

客户端提供“实时投屏”和“区域投屏”两种模式。区域投屏先取得屏幕或窗口的冻结预览，用户鼠标框选后持续实时采集该区域；运行中可以重新框选，取消不会改变当前区域，点击 Stop 后设备恢复主页。

Ubuntu/Debian 首次构建需要安装 Tauri、WebKitGTK 和 PipeWire 依赖，然后执行：

```bash
cd companion_display/desktop_client
npm ci
npm run tauri:dev
```

生成发布包：

```bash
npm run tauri:build
```

仅验证前端与 Rust 核心：

```bash
npm run build
cd src-tauri
cargo test
cargo clippy --all-targets
```

完整使用说明和故障排查见 [PC 客户端实现](docs/pc-client.md)。

## ESP32-S3 固件

在仓库根目录执行：

```bash
cd companion_display/firmware/esp32s3
./dev.sh build
```

构建、烧录、配网和设备诊断见 [ESP32-S3 设备固件](firmware/esp32s3/README.md)。
