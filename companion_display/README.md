# Companion Display

本目录统一保存 Companion Display v3 的公共协议、PC 客户端和设备固件。各端通过相同协议协作，但保留独立构建入口；公共客户端不得依赖某个设备工程的源码、构建产物或固定屏幕尺寸。

## 目录

- `PROTOCOL.md`：Companion Display v3 线上协议的权威定义。
- `docs/pc-client.md`：PC 客户端架构、数据流和排障说明。
- `desktop_client/`：Tauri 2 + Vue 3 桌面客户端。
- `firmware/esp32s3/`：Waveshare ESP32-S3-Touch-AMOLED-1.8 设备固件。
- `firmware/esp32p4/`：JC1060P470（JD9165 + GT911 + ESP-Hosted/C6）设备固件。

WDP/WebRTC 位于仓库顶层 `wireless_display/`，它与本目录的 WebSocket/JPEG/UDP v3 协议相互独立，不共享版本号或兼容承诺。

## 设备适配要求

兼容设备必须实现 `PROTOCOL.md` 定义的 `/api/status`、WebSocket 控制通道、`CDP1` 完整帧，以及所声明的 TCP/UDP 视频通道。设备通过 `hello_ack.width`、`height` 和 `max_frame_bytes` 声明当前会话目标，客户端据此完成采集缩放、方向处理和 JPEG 限流。带触摸屏的设备可以在协商后经 `touch_events` 上报原始触摸事件，由客户端完成手势识别与鼠标注入。

当前设备默认能力如下。表中的帧上限包含 24 字节 `CDP1` 头；实际会话仍以 `hello_ack` 为准。

| 设备 | 网络 | 视频 / 触摸坐标 | TCP 完整帧上限 | UDP 有效完整帧上限 | 构建根 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 Waveshare AMOLED 1.8 | 芯片内置 2.4 GHz Wi-Fi | `368x448` / `368x448` | `102424` 字节 | `102424` 字节 | `firmware/esp32s3` |
| ESP32-P4 JC1060P470 | ESP32-C6 + ESP-Hosted SDIO | `1024x600` / `1024x600` | `393240` 字节 | `153600` 字节 | `firmware/esp32p4` |

公共客户端从 `hello_ack` 读取视频尺寸、帧上限和触摸坐标空间，不识别或写死具体板卡。P4 的 TCP JPEG payload 上限为 384 KiB；UDP 仍受 128 个、每个 1200 字节分片的限制。

S3/P4 的 UDP 会话只用 UDP 承载 JPEG `CDU1` 分片，协商、心跳、触摸和 `udp_feedback` 始终走 TCP/WebSocket。新固件协商 `latest_frame_ack`：PC 同时只发送一帧，在设备通过 TCP 报告 `submitted`、`unrecoverable` 或 `submit_failed` 后再发送最新画面，不重传旧帧。链路拥塞时先降低 6～20 Mbit/s pacing 和实际 FPS，达到 5 FPS 且持续失败后才收紧 JPEG 帧预算，实际质量下限保持 30。旧固件缺少流控字段时客户端保留兼容发送路径。

## PC 客户端

主界面按“设备、画面来源、投屏”三步组织设置，设置项始终可见，避免在多个折叠面板之间切换。左侧集中显示设备画面预览、当前就绪状态和传输/FPS/设备耗时/丢帧四项关键指标；UDP/FEC、缩放、JPEG 和各阶段耗时可按需展开。设备诊断和事件日志默认收起，FPS 与 JPEG 质量同时提供滑块和精确数值输入，投屏期间仍可实时调整。

客户端提供“实时投屏”和“区域投屏”两种模式。区域投屏先取得屏幕或窗口的冻结预览，用户鼠标框选后持续实时采集该区域；Linux 自动恢复上次来源时，可在全屏框选层直接更换屏幕或窗口。运行中可以重新框选，取消不会改变当前区域，点击 Stop 后设备恢复主页。开启“触摸控制”后，投屏期间在设备屏幕上单击、拖拽、长按（右键）和右缘滑动（滚轮）会反向操控 PC 鼠标，详见 [PC 客户端实现](docs/pc-client.md)。

Linux 首次授权或重新选屏时，客户端在 `hello_ack` 后先取得并编码首帧，首帧就绪后才发送 `start_stream`，避免 Portal 操作消耗设备的 10 秒无帧超时。PipeWire 回调裁剪、缩放并覆盖发布最新画面，变化画面的锐化与 JPEG 编码在发送线程执行。PipeWire 优先协商最高 30 FPS，并为不接受该范围的 Portal 后端保留宽松格式回退。

Ubuntu/Debian 首次构建需要安装 Tauri、WebKitGTK 和 PipeWire 依赖，然后执行：

```bash
cd companion_display/desktop_client
npm ci
npm run tauri:dev
```

`tauri:dev` 保留调试信息，不能代表发布性能。需要热更新并按发布优化级别复测 CPU 时使用：

```bash
npm run tauri:dev:release
```

生成发布包：

```bash
npm run tauri:build
```

Linux 默认同时生成 Debian、RPM 和 AppImage 包，分别位于 `src-tauri/target/release/bundle/deb/`、`rpm/` 和 `appimage/`。工程内置方形应用图标及 Tauri 桌面图标配置，三类包应在同一次构建中完成。首次生成 AppImage 需要从 GitHub 下载 `linuxdeploy`、打包插件和 AppImage runtime，构建环境必须能够访问对应发布资源。

仅验证前端与 Rust 核心：

```bash
npm run build
cd src-tauri
cargo test
cargo clippy --all-targets
```

完整使用说明和故障排查见 [PC 客户端实现](docs/pc-client.md)。

## 设备固件

在仓库根目录执行：

```bash
cd companion_display/firmware/esp32s3
./dev.sh build

cd ../esp32p4
./dev.sh build
```

构建、烧录、配网和设备诊断见 [ESP32-S3 设备固件](firmware/esp32s3/README.md) 和 [ESP32-P4 设备固件](firmware/esp32p4/README.md)。
