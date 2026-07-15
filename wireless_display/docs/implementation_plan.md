# Wireless Display 实施计划

## 1. 交付原则

WDP 按“规范冻结、参考核心、互操作原型、平台产品化”推进。每一阶段必须通过验收门槛后才能把行为写成稳定接口；平台性能限制通过能力档表达，不反向污染协议核心。

## 2. 阶段 0：协议文档与模式

交付内容：

- `README.md`、架构、协议、ESP 能力档和本实施计划。
- `protocol/wdp.cddl` 及后续增加的有效/无效 CBOR 测试向量。
- 发现、配对、信令、DataChannel、媒体、输入、遥测、错误和安全威胁模型。

完成标准：

- 文档示例与 CDDL 字段、编号和限制一致。
- 所有权限、超时、状态转换、断线清理和版本规则都能转化为测试。
- 至少完成一次针对身份冒充、凭据泄露、输入注入、麦克风滥用和资源耗尽的评审。
- 冻结标签为 `WDP 1.0-draft1`；冻结后不直接重定义既有字段。

## 3. 阶段 1：Rust 参考核心

建议 workspace：

```text
crates/wdp-core       能力、状态机、错误和平台接口
crates/wdp-codec      CBOR 编解码与 CDDL 测试向量
crates/wdp-signaling  JSON 信令模型，不包含具体网络服务器
crates/wdp-capi       稳定 C ABI 与头文件
tests/interop         跨语言黄金向量和状态机场景
```

公共 Rust 接口只暴露拥有明确生命周期的数据类型：`EndpointCapabilities`、`SessionConfig`、`SessionEvent`、`WdpMessage`、`ProtocolError`、`SessionMachine` 和 `CapabilityMatcher`。WebRTC、发现、凭据和信令通过 trait 注入，不在核心中选择具体运行时。

C ABI 使用 `wdp_*_handle` 不透明句柄、`ptr + len` 字节切片、显式错误码和用户上下文回调。提供创建/销毁、输入消息、拉取输出事件和序列化/反序列化 API；不得跨 ABI 暴露 Rust panic、引用、容器或异步 future。

验证：

- `cargo test --workspace`、Clippy、内存/线程 sanitizer 可用平台检查。
- CDDL 黄金向量双向一致，未知可选字段保留互操作，未知必需特性正确拒绝。
- 状态机覆盖正常建立、拒绝、重协商、ICE restart、generation 更新、超时和异常清理。
- 对 JSON、CBOR 和 C ABI 长度边界进行 fuzz。

## 4. 阶段 2：桌面互操作工具

先实现无产品 UI 的命令行参考端点，用于验证协议而不是替代最终客户端：

- mDNS 浏览/发布、二维码 URI 导入、证书固定和凭据存储。
- 内置 HTTPS/WSS 响应端和 WSS 发起端。
- 可替换 `PeerBackend`，首个 Linux 后端优先评估 GStreamer WebRTC；测试后端用于无媒体状态机测试。
- 测试图、系统音频、麦克风、触摸模拟器和遥测查看器。
- 输出机器可读互操作报告，不把平台日志当协议结果。

门槛为两实例在同一主机和两台 Linux 主机间完成视频、双向音频、两个 DataChannel、ICE restart 与权限撤销。

## 5. 阶段 3：ESP WebRTC 原型

基于固定版本的官方 `esp-webrtc-solution` 实现 `WDP-ESP/1`：

- 先以 ESP32-P4 或官方已验证板卡跑通信与媒体路径，再移植 Waveshare ESP32-S3 AMOLED。
- 将 WDP 会话、配对、CBOR、显示、触摸和音频拆成独立组件。
- 实测生成 capability，不复用当前 Companion Display 的固定尺寸或硬编码布尔能力。
- 按 `esp-webrtc-profile.md` 完成 30 分钟稳定性、5% 丢包、双向音频、触摸延迟、重连和安全用例。

ESP 原型失败只影响 ESP 能力档，不修改 WDP 通用接口。旧 `companion_display` 保持可用且不承担兼容回退。

## 6. 阶段 4：RK 与移动端

- RK3506/Linux：使用可用的 V4L2、DRM、ALSA 和硬件 H.264 能力，通过 GStreamer/libwebrtc 后端接入；能力声明来自驱动查询和持续性能测试。
- Android：以系统 WebRTC、MediaProjection、AudioRecord/AudioTrack 和 Keystore 接入，Rust 核心通过 C/JNI 边界复用。
- iOS：以系统采集、音频会话、Keychain 和 WebRTC framework 接入，Rust 核心通过 C/Swift 边界复用。
- 各平台都必须能作为发起端或响应端，具体媒体方向由权限和能力决定。

互操作矩阵至少覆盖 Linux↔RK、Linux↔Android、Linux↔iOS、Linux↔ESP32-S3/P4。平台新增可选能力不得破坏只实现 WDP 基线的端点。

## 7. 阶段 5：产品客户端

协议与至少两个设备平台稳定后再建设产品 UI：设备发现、二维码配对、权限管理、来源选择、轨道控制、质量策略、诊断和凭据撤销。UI 不直接解析 SDP 或平台私有状态，只消费参考核心事件。

## 8. 性能与可靠性目标

以下是实现验收目标，不是协议合规的固定硬件要求：

- 支持硬件编解码的平台以 1080p60 为完整能力档目标；端点必须按真实能力降级。
- ESP32-S3 首个显示目标为 `368x448 @ 25 FPS`，最终数值以持续测试为准。
- 空闲局域网触摸端到端延迟目标小于 50 ms，交互视频玻璃到玻璃延迟目标小于 150 ms。
- 5% 随机丢包和 50 ms 抖动下无无界队列，网络恢复后 5 秒内恢复实时状态。
- 双向音频持续 30 分钟无累计时钟漂移导致的中断，无持续音频欠载。
- 每个平台连续运行 8 小时无持续内存增长，异常断线后麦克风、摄像头、输入和显示资源全部释放。

## 9. 暂不实施

- SFU、MCU、一对多同步播放和录制服务。
- 云账户、跨互联网设备目录和 TURN 服务运维。
- 文件传输、固件升级、剪贴板文件和远程 shell。
- 自定义编解码器、私有 RTP 扩展或绕过 WebRTC 安全层的媒体通道。
