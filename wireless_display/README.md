# Wireless Display

`wireless_display` 用于设计和实现硬件无关的无线投屏协议 Wireless Display Protocol（WDP）。它面向桌面系统、RK 系列 Linux 设备、Android/iOS 和资源受限的 ESP32-S3/P4，不把任一平台、固定分辨率或固定媒体编码写入协议核心。

当前状态为 `WDP 1.0-draft1` 文档阶段。工程暂不包含可运行客户端或设备固件。

## 目标

- 任意端点按能力声明发送或接收视频、播放音频和麦克风音频。
- 接收端可向来源端回传触摸、指针、键盘、文字输入和设备遥测。
- 使用 WebRTC 提供标准媒体协商、DTLS-SRTP、音画同步、拥塞控制、NACK 和 ICE。
- 局域网无需云服务即可发现、配对和直连，并为未来 STUN/TURN 与外部信令保留接口。
- 通过统一模式、测试向量和参考状态机保证 Rust、C、移动端及 Web 实现互操作。

## 已确定的技术路线

| 项目 | 决策 |
| --- | --- |
| 媒体基础 | WebRTC PeerConnection |
| 端点模型 | 能力驱动的对等端，不固定 PC/设备角色 |
| 局域网发现 | DNS-SD/mDNS `_wdp._tcp.local` |
| 局域网信令 | 接收端内置 HTTPS/WSS；外部信令为可选扩展 |
| 首次配对 | 二维码携带设备身份、一次性密钥和 TLS 证书指纹 |
| 应用消息 | WebRTC DataChannel 上的 CBOR，使用 CDDL 定义 |
| 参考核心 | Rust，提供稳定 C ABI；WebRTC 引擎通过适配接口接入 |
| 基线媒体 | H.264 视频与 Opus 音频，其他编码按能力协商 |

## 文档

- [架构设计](docs/architecture.md)：分层、数据流、端点模型和安全边界。
- [WDP 1.0 协议](docs/protocol.md)：发现、配对、信令、DataChannel、媒体和错误处理。
- [ESP WebRTC 能力档](docs/esp-webrtc-profile.md)：ESP32-S3/P4 的官方组件适配与验证门槛。
- [实施计划](docs/implementation_plan.md)：从协议冻结到参考核心和各平台适配的阶段安排。
- [CBOR 模式](protocol/wdp.cddl)：DataChannel 消息的权威结构定义。

## 与 Companion Display 的关系

`companion_display/firmware/esp32s3` 是针对单块 AMOLED 屏的既有协议 v2 固件，使用 WebSocket、JPEG 和私有 UDP 分片。WDP 是独立的新协议，不继承其版本号、帧结构或兼容承诺。旧工程继续独立维护；只有在 WDP 的 ESP 能力档通过实机验证后，才评估迁移或并行支持。

## 非目标

- WDP 不定义新的音视频编解码器、RTP 变体或私有拥塞控制算法。
- `draft1` 不包含 SFU、一对多同步播放、远程账户系统或云服务部署。
- 协议文档冻结前不承诺具体平台的性能、分辨率或并发会话数量。
