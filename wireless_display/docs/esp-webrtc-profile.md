# ESP WebRTC 能力档

## 1. 目的

本文定义 ESP32-S3/P4 接入 WDP 的受限能力档 `WDP-ESP/1`。它不改变 WDP 核心协议，只约束 ESP 实现必须支持的 WebRTC 子集、资源声明和验证门槛。

截至 2026-07-15，乐鑫官方 `esp-webrtc-solution` 提供以下可复用基础：

- `esp_peer`：ICE、DTLS、SRTP、SCTP、RTP NACK、jitter buffer、TURN 和 DataChannel。
- 媒体方向支持 send、recv 和 sendrecv。
- 音频支持 G.711 A-law/μ-law、Opus；视频支持 H.264、MJPEG。
- `doorbell_local` 演示本地 HTTPS 信令、单向视频和双向音频。
- `webrtc_usb_camera` 演示浏览器向 ESP32-S3/P4 下行 WebRTC 视频。
- `peer_demo` 说明最小 PeerConnection 可在无 PSRAM 的 ESP32-S3 上运行，但这不代表完整解码和渲染负载已满足。

上游仓库：<https://github.com/espressif/esp-webrtc-solution>。实现阶段必须固定经过验证的 commit，不得直接依赖浮动 `master`。

## 2. WDP-ESP/1 最低能力

- 通过 `_wdp._tcp.local` 发布单个 HTTPS/WSS 服务，默认 `max_sessions=1`。
- 支持二维码证书固定、一次性配对密钥、长期凭据和撤销。
- 支持 WDP JSON 信令、Trickle ICE、host candidate 和 ICE restart。
- 支持 `wdp.control.v1` 与 `wdp.realtime.v1` 两个 DataChannel。
- 视频接收至少声明 H.264 Constrained Baseline、`packetization-mode=1`；实际 level、尺寸、帧率、像素率和码率必须来自实测。
- 音频至少声明设备能够稳定运行的一种 Opus 配置；G.711 可作为轻量补充，但不能替代 WDP 通用 Opus 基线。
- 支持 `interactive` 延迟模式、单显示面、触摸快照发送、设备状态和基础资源遥测。
- 不要求摄像头视频、IME、Consumer Control、多会话、simulcast、SVC 或公网 TURN。

不具备某资源的板卡必须省略能力，不得返回硬编码 `true`。ESP32-P4 若通过独立 Wi-Fi 芯片联网，网络拓扑不得暴露到 WDP 信令或媒体语义中。

## 3. 组件映射

| WDP 层 | ESP 实现建议 |
| --- | --- |
| mDNS | ESP-IDF mDNS 组件 |
| HTTPS/WSS | `esp_http_server` TLS 配置或经过评审的轻量服务 |
| 配对凭据 | NVS 加密分区；设备证书和私钥使用受保护存储 |
| PeerConnection | 官方 `esp_peer` 默认实现 |
| 捕获与渲染 | `esp_capture`、`av_render` 或板级显示/音频适配器 |
| H.264 | 乐鑫 `esp_h264` 或平台硬件解码路径 |
| DataChannel | `esp_peer_send_data` 与 `on_data` 回调 |
| CBOR | 有长度和深度限制的 C 实现，按 `wdp.cddl` 校验 |

WDP 会话状态机应位于独立组件，不直接耦合 LCD、触摸、音频 codec 或 Wi-Fi 驱动。板级组件只实现 surface、media、input 和 telemetry 接口。

## 4. 资源与流控要求

- 能力中的上限必须取最差可持续结果，不使用单帧或短时峰值。
- 解码、渲染和音频队列必须有界；`interactive` 模式在过载时丢弃旧视频，不阻塞触摸和音频。
- control 最低接收消息为 4096 字节，realtime 最低为 1200 字节；更大消息必须在能力中明确。
- 触摸使用状态快照，活动期间至少 100 ms 重发；接收或发送任务不得等待视频解码完成。
- 遥测至少报告空闲内存、最低空闲内存、解码队列、渲染耗时、音频欠载、Wi-Fi RSSI 和温度（硬件可用时）。
- 证书、SDP、CBOR 和媒体缓冲必须设置独立上限，资源耗尽返回标准错误并安全释放会话。

## 5. ESP32-S3/P4 可行性门槛

首个原型必须依次通过，未通过前不得在能力档中承诺对应功能：

1. PeerConnection、DTLS-SRTP、两个 DataChannel 连续运行 30 分钟，无持续堆内存下降。
2. PC 向 ESP 下行 H.264，正确协商尺寸和 profile；S3 目标场景先验证 `368x448 @ 25 FPS`，P4 再验证更高档位。
3. 视频过载或 5% 模拟丢包时队列保持有界，恢复后不持续播放旧画面。
4. 下行音频与视频并行时无持续音频欠载；再增加设备麦克风上行，验证双向音频。
5. 触摸端到端延迟在空闲局域网目标低于 50 ms，视频拥塞时仍能释放所有接触点。
6. 网络切换或短时断网可通过 ICE restart 恢复；完整重建后旧 generation 消息被拒绝。
7. 二维码配对、证书指纹不匹配、密钥重放、凭据撤销和权限拒绝均符合协议。

若 S3 无法同时满足视频、双向音频和 DataChannel 资源门槛，应缩小其能力声明或继续使用旧 Companion Display 协议；不得降低 WDP 安全、媒体或消息语义。P4/RK/手机能力不受 S3 上限约束。

## 6. 版本与依赖风险

- 上游示例主要声明 ESP-IDF v5.4 或 master；`esp_peer` 文档同时给出 IDF 6.0 的 DTLS 配置。原型需分别记录实际 IDF、组件 commit 和 sdkconfig。
- 浏览器与平台 WebRTC 对 MJPEG 支持不一致，因此 MJPEG 只能作为双方明确支持的可选 codec，不能作为通用基线。
- H.264 解码、旋转、色彩转换和 LCD 提交可能竞争 PSRAM/带宽，必须用端到端测量确定能力，而不是只测网络接收。
- 官方 demo 的信令和产品权限模型仅供参考；WDP 必须使用自己的证书固定、凭据和消息规范。
