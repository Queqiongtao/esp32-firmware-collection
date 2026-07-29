# Companion Display PC 端实现

本文说明桌面客户端的安装、界面结构、前后端边界、画面处理、传输策略和性能诊断。通信字段与帧格式以 [`PROTOCOL.md`](../PROTOCOL.md) 为准。

## 技术栈与构建

客户端位于仓库根目录 `companion_display/desktop_client`：

- 前端：Vue 3、TypeScript、Pinia、Naive UI、Vite；
- 桌面容器：Tauri 2；
- 后端：Rust；
- Linux 采集：XDG Desktop Portal + PipeWire；
- 非 Linux 采集：XCap 枚举显示器和窗口；
- 编码与传输：`image`、`tungstenite`、`crc32fast`；
- 触摸注入：Linux 走 XDG RemoteDesktop Portal（与采集同一会话）；Windows/macOS 走 `enigo`（SendInput / CGEvent）。

Ubuntu/Debian 依赖：

```bash
sudo apt install build-essential pkg-config curl clang libclang-dev \
  libwebkit2gtk-4.1-dev libgtk-3-dev libpipewire-0.3-dev \
  libssl-dev libayatana-appindicator3-dev librsvg2-dev
```

`clang` 和 `libclang-dev` 用于构建仓库内 vendor 的 PipeWire/libspa 绑定。开发与发布命令：

```bash
cd companion_display/desktop_client
npm ci
npm run tauri:dev
```

`tauri:dev` 使用带调试信息的开发构建；工程为应用代码启用 `opt-level=1`、依赖启用 `opt-level=2`，降低图像热路径在日常调试时的额外开销。`tauri:dev:release` 保留前端开发服务器和热更新，但 Rust 后端使用 release 构建，性能评估以该模式或正式安装包为准：

```bash
npm run tauri:dev:release
```

生成发布包：

```bash
npm run tauri:build
```

发布二进制位于 `src-tauri/target/release/`。Linux 默认在其 `bundle/deb/`、`bundle/rpm/` 和 `bundle/appimage/` 子目录分别生成 Debian、RPM 和 AppImage 包。工程使用 `src-tauri/icons/app-icon.svg` 作为高分辨率图标母版，并在 `bundle.icon` 显式列出 Tauri 生成的桌面图标；若修改品牌图标，应重新执行 `npm exec tauri icon -- src-tauri/icons/app-icon.svg --output src-tauri/icons` 后再打包，避免 AppImage 因找不到方形图标而失败。首次生成 AppImage 还会从 GitHub 下载 `linuxdeploy`、打包插件和 AppImage runtime；`Failed to download runtime` 或状态码 `0` 表示构建环境的网络或代理无法取得发布资源，不是应用编译失败。默认窗口为 1280×800，最小 720×560。

Linux 启动时会在 WebKitGTK 初始化前设置 `WEBKIT_DISABLE_DMABUF_RENDERER=1`，规避部分 Wayland、虚拟 GPU 或显卡驱动下的 DMA-BUF 界面合成杂块；该设置不影响 PipeWire 采集和视频发送。

### 界面语言

顶部栏提供中文和英文切换，不需要重启客户端，也不受推流锁定状态影响。首次运行时读取浏览器/WebView 报告的系统语言：语言代码以 `zh` 开头时默认 `zh-CN`，其他情况默认 `en-US`。用户选择保存在 `companion-display.locale.v1`，后续启动优先使用保存值。

本地化使用项目内置的类型化语言资源，不引入第三方 i18n 运行库。按钮、标签、方向选项、校验提示、设备状态、常用流状态和结构化事件日志均可即时切换；操作系统提供的显示器名称、窗口标题以及 Rust 返回的 PipeWire、网络和协议底层诊断保留原文，便于直接搜索错误。

## 前后端结构

```text
Vue 组件
  -> Pinia stores / composables
  -> Tauri invoke + event
  -> Rust StreamState 与工作线程
  -> 采集 / JPEG / WebSocket / UDP
```

### 前端

主界面由顶部状态栏、左侧预览工作区、右侧三步投屏设置和底部事件日志组成。投屏设置按“设备、画面来源、投屏”从上到下排列，不再折叠必要输入；设备地址旁可直接检查连接，设备详细诊断按需展开，来源刷新与选择位于同一行。投屏模式使用分段控件，FPS 和 JPEG 质量同时提供滑块与数值步进输入，底部就绪提示会直接说明当前缺少的条件，主按钮随空闲、区域投屏和运行中状态切换为开始、重新框选或停止。预览标题栏另有常驻的开始/停止按钮，右侧设置滚动到任何位置都能控制主生命周期。

预览工作区顶部固定显示当前就绪或运行状态，底部默认保留传输方式、FPS、设备耗时和丢帧四项关键指标。点击“更多指标”后显示 UDP/FEC、缩放比例、JPEG 实际质量与容量、采集、编码、发送和设备替换帧等诊断数据；性能受限时仍显示明确告警。事件日志默认折叠，排障时再展开。

窗口宽度小于约 1100px 时从双列切换为单列：预览和关键指标在上，三步设置及日志在下方通过页面纵向滚动访问。720px 以下会压缩顶部工具和页面留白，520px 以下顶部状态换行；控件保持完整，不依赖横向滚动。

预览组件通过 `ResizeObserver` 测量工作区中扣除标题和指标后的实际可用区域，并计算可容纳的最大等比例矩形。比例来自当前流协商的设备宽高；尚未启动时使用 `/api/status` 的宽高，尚未检测设备时才回退到 S3 的 `368:448`。顶部朝左或朝右时交换宽高；窗口、指标栏、日志或语言文本尺寸变化时会重新测量。预览图继续使用 `object-fit: contain`，不拉伸、不裁剪，容器设置明确裁切边界以避免首次测量时影响相邻区域。默认最小窗口 `720×560` 为完整布局支持下限，更小的窗口尺寸不保证全部控件同时可见。

主要状态职责：

| 模块 | 职责 |
| --- | --- |
| `settings` | 地址、配对码、投屏模式、FPS、质量、显示方向、传输方式和 Portal 选择持久化 |
| `device` | 设备可达性、协议版本、支持传输和 UDP 诊断 |
| `stream` | 流生命周期、指标窗口、性能受限状态和预览 |
| `logs` | 最近 500 条探测、来源刷新、启停和错误事件 |
| `theme` | `auto`、`light`、`dark` 主题持久化 |
| `locale` | 系统语言检测、中英文资源、语言切换和选择持久化 |
| `useStreamLifecycle` | 探测、防抖、参数更新、启停和轮询 |
| `useTauriEvents` | 消费 Rust 的状态与预览事件 |
| `usePreviewSink` | 将预览 JPEG 转换为前端可显示资源 |

设置使用 `companion-display.settings.v1` 保存；主题使用独立 key。采集来源 `sourceId` 不持久化：Linux 启动后重新枚举，并在当前来源为空或失效时自动选择 `linux:portal`；非 Linux 仍由用户选择显示器或窗口。

### Tauri 接口

前端调用的命令：

| 命令 | 作用 |
| --- | --- |
| `list_capture_sources` | 返回测试源、Portal 或 XCap 来源 |
| `probe_wifi_device` | 请求设备 `/api/status` 并解析诊断字段 |
| `get_stream_status` | 返回 Rust 内保存的最近流状态 |
| `start_wifi_stream` | 校验参数、打开采集与控制连接、启动发送线程 |
| `prepare_region_selection` | 打开来源并生成一次性全屏框选预览 |
| `cancel_region_selection` | 取消并释放尚未启动的区域采集会话 |
| `prepare_region_reselection` | 从活动来源取得重新框选预览 |
| `replace_stream_region` | 在不中断连接的情况下更新实时采集区域 |
| `set_stream_params` | 运行中原子更新 FPS 和 JPEG 质量 |
| `stop_wifi_stream` | 设置停止标志，由发送线程完成协议停止和资源释放 |

Rust 向前端发送：

| 事件 | payload | 作用 |
| --- | --- | --- |
| `stream-status` | `StreamEvent` | 生命周期、FPS、耗时、丢帧和替换统计 |
| `stream-preview` | Base64 JPEG | 最近成功编码画面的低频预览 |

前端每 300 ms 轮询一次 `get_stream_status`，UDP 流运行时每 2 秒探测一次设备状态。地址或配对码输入约 800 ms 后自动探测；FPS 和质量修改约 300 ms 后下发。

## 设备连接与生命周期

用户可输入裸 IP、`ws://` 地址或完整 `/stream` 地址，前端会规范化为：

```text
ws://DEVICE_IP[:PORT]/stream
```

启动时 Rust 追加 `?token=PAIRING_CODE`，使用 5 秒 TCP 建连、读写超时，并启用 `TCP_NODELAY`。客户端先发送 `hello` 取得目标尺寸、帧上限和传输参数，随后完成 Portal 授权、首帧采集、图像增强和 JPEG 编码；只有首帧可立即发送时才请求 `start_stream`。这样 Portal 对话框和首次图像处理不会消耗设备从 `stream_started` 起计算的 10 秒无帧超时。地址、配对码、来源、投屏模式、显示方向和传输方式在流运行期间锁定；FPS 与 JPEG 质量通过原子变量每轮读取，可以动态修改。

当前两个固件的默认协商结果如下；客户端不按设备名称分支，所有值均来自 `/api/status` 与 `hello_ack`：

| 设备 | 视频尺寸 | 触摸坐标空间 | TCP 完整帧上限 | UDP 有效完整帧上限 |
| --- | --- | --- | --- | --- |
| ESP32-S3 | `368x448` | `368x448` | `102424` 字节 | `102424` 字节 |
| ESP32-P4 | `1024x600` | `1024x600` | `393240` 字节 | `153600` 字节 |

触摸坐标空间独立解析自 `hello_ack.touch`。客户端将其归一化到视频物理尺寸，因此未来设备的触摸分辨率可以与视频尺寸不同；触摸描述缺失、越界或与 capability 矛盾时拒绝启动。

Stop 不是立即杀死线程：前端进入 `stopping`，Rust 设置停止标志，发送线程发送 `stop_stream`、关闭 WebSocket、停止采集并释放状态，最后发出 `stopped`。

## 画面来源与处理

### 区域投屏

投屏面板可在“实时投屏”和“区域投屏”间切换。区域模式先选择屏幕或窗口；Linux 使用 Screencast Portal 和 PipeWire，Windows、macOS 使用 XCap。客户端取得来源的一帧作为冻结背景并进入全屏框选层，矩形确认后按设置的 FPS 持续采集、缩放、编码和发送该区域。

首次启动先检查设备，再打开来源和框选层；取消时释放待用采集会话，不连接设备。Linux 使用 restore token 直接恢复来源时，框选层显示“更换屏幕或窗口”：点击后退出全屏、释放当前待用会话、清除 token，并立即重新打开 Portal 系统选择框。活动期间可从同一来源重新框选，确认后更新 PipeWire transform 或 XCap Crop，取消则继续原区域；活动流不允许直接更换来源。选区坐标以来源像素表示，最小为 `16x16`；内容按设备逻辑画布等比缩放并补边。静态内容仍沿用通用的 1 FPS 缓存帧保活策略，但区域本身是持续实时流。

### 测试源

`Static color bars` 直接生成设备尺寸的八色条，不依赖屏幕采集权限，适合验证：

```text
PC JPEG 编码 -> 协议封装 -> 网络 -> 设备解码 -> 设备显示
```

### Linux Portal/PipeWire

Linux 只在系统 Portal 对话框中选择屏幕或窗口，不逐帧调用 XCap。客户端要求 Portal 支持嵌入式光标；光标作为画面的一部分参与后续 Crop 和缩放。

Portal 使用 `PersistMode::ExplicitlyRevoked` 请求持久化授权，并将 `start` 响应中的 restore token 通过 `portal-restore-token` 事件交给前端，保存在 `companion-display.settings.v1`。后续 Start 会把 token 传回 `select_sources`，桌面后端支持时恢复上次屏幕或窗口。token 不会通过设备 WebSocket 或 UDP 发送。

Source 面板显示当前是否已有记忆选择。停止投屏后点击“重新选择”会清除本地 token，下一次 Start 重新打开系统对话框；区域投屏已进入全屏冻结预览时，也可以点击“更换屏幕或窗口”立即清除 token 并重新打开选择框。共享对象消失、权限被系统或用户撤销、token 失效或 Portal 后端不支持持久化时，系统仍可要求重新选择；客户端不绕过 Portal 安全策略。

PipeWire 回调直接生成目标尺寸图像并连同缩放比例、有效内容矩形一起发布，避免复制完整桌面；锐化和 JPEG 编码不在回调线程执行。选择窗口时有效区域按以下优先级确定：

1. 标准 `SPA_META_VideoCrop`；
2. BGRA/RGBA Alpha 非透明边界；
3. 完整协商帧。

GNOME Portal 可能为单窗口返回全屏大小透明画布，因此 `logical_size` 或 PipeWire negotiated size 不一定等于窗口主体。日志中的 `PipeWire VideoCrop` 和 `Capture layout.crop` 才是实际处理区域。

空帧、损坏帧、异常 stride 或布局尚未稳定的启动过渡帧会被跳过。共享获批后 10 秒仍没有有效首帧才报告超时。

### 非 Linux XCap

非 Linux 构建会枚举 monitor 和非空标题 window。来源 ID 使用 `monitor:N` 或 `window:N`。来源列表基于枚举序号，窗口消失或顺序变化后需要刷新并重新选择。

### 区域、缩放与补边

实时投屏使用自动识别后的完整有效来源；区域投屏的框选坐标相对于该有效来源，并始终在旋转前解释。PC 客户端不再提供独立的手动 Crop 数值设置。

`Display orientation` 表示设备物理顶部所在方向。持久化枚举沿用原值，但选项不再假设设备原生是竖屏：

| 选项 | 逻辑画布 | PC 编码补偿 |
| --- | --- | --- |
| `Top edge up` / 顶部朝上 | `width x height` | 不旋转 |
| `Top edge left` / 顶部朝左 | `height x width` | 顺时针旋转为原生帧 |
| `Top edge down` / 顶部朝下 | `width x height` | 旋转 180° |
| `Top edge right` / 顶部朝右 | `height x width` | 逆时针旋转为原生帧 |

选定区域按逻辑画布比例缩放：

- 不拉伸；
- 不裁掉选定内容；
- 宽高比不一致时居中补最少黑边。

处理顺序固定为：窗口有效区域识别、应用区域投屏选区、按 `hello_ack.width/height` 建立逻辑画布并等比缩放和补边、方向补偿旋转、变化检测、可选锐化、JPEG 编码。实时投屏跳过选区步骤。设备端不执行缩放或旋转，因此 PC 始终输出完整的协商设备尺寸。

Linux PipeWire 把首选帧率设为用户值，优先协商最高 30 FPS；为兼容不接受该范围的 Portal 后端，同时提供最高 1000 FPS 的宽松回退格式，并在日志打印实际协商结果。每个有效回调使用按裁剪和方向缓存的定点双线性采样表生成目标画面，覆盖尚未消费的旧帧，使发送线程始终取得最新画面。目标坐标使用起始位置和横纵步长表达，不建立逐像素偏移表；逻辑坐标直接写入物理输出画布，不复制完整桌面。每 5 秒日志输出回调率、实际转换率以及平均/最大转换耗时。非 Linux XCap 和测试源先生成逻辑画布，再旋转到设备画布。

当来源到有效画面区域的缩放比例低于 90% 时，发送线程在缩放后使用固定 `3x3` 整数亮度核执行轻量锐化；处理范围不包含补出的黑边。实现复用亮度和横向卷积工作区，并利用该核可分离的特性减少邻域运算，输出规则和锐化强度不变。变化检测在锐化前完成，因此静态画面直接复用缓存 JPEG，不重复消耗锐化和编码时间。原尺寸、放大画面和测试色条不锐化。

## JPEG 与帧生成

FPS 范围为 1～30，用户可选 JPEG 质量范围为 30～95。新安装默认 TCP、20 FPS、质量 85；已有持久化设置保持原值。TCP 和 UDP 的实际编码质量下限均为 30；UDP 拥塞时先降低 pacing 与实际 FPS，只有达到 5 FPS 且连续失败后才收紧帧字节预算。

TCP 编码上限为 `hello_ack.max_frame_bytes - 24`；UDP 先使用设备协商上限，再受链路控制器的运行时分片预算约束，并继续校验 `max_payload * 128`。首帧按 5 粗调并细化最后区间，找到不超过上限的最高质量；后续变化帧直接复用上次成功质量，通常只编码一次。只有当前帧超限时才按 5 降级重试；存在至少 10% 容量余量时，每秒最多探测一次高一级质量。质量 30 仍超限时暂停重复编码；UDP 可按 1 FPS 发送最近成功的缓存帧维持会话。成功 JPEG 被封装为 24 字节 `CDP1` 头和 payload，帧头宽高使用协商值，CRC 使用与设备 `esp_crc32_le(0, ...)` 兼容的算法。

客户端对目标尺寸图像计算指纹。画面不变、采集端暂时没有新帧或 UDP 当前预算无法编码变化帧时，复用最后成功编码的 JPEG，并以 1 FPS 发送；每次重发只更新 `CDP1.sequence`，JPEG payload 与 payload CRC 不变。失败画面的重新编码与错误上报最多每秒一次，避免弱网期间消耗过多 CPU 或刷屏。这样既减少静态带宽，也尽可能避免设备无帧超时。

预览只在成功生成 `CDP1` 后提取 JPEG，最多每秒发送一次 Base64 事件。前端 canvas 按设备摆放方向反向旋转：顶部朝左或朝右时交换协商宽高，顶部朝下时旋转 180°，预览始终保持便于观看的方向。预览频率与视频 FPS 解耦。

## TCP 发送

TCP 模式把完整 `CDP1` 作为 WebSocket binary message 发送。每帧发送后等待相同 sequence 的 `frame_presented`：

```text
发送一帧 -> 设备排队/解码/显示提交 -> frame_presented -> 发送下一帧
```

因此可靠连接不会积压多帧旧画面。设备耗时由 `queue_wait_us + jpeg_decode_us + lcd_wait_us + lcd_submit_us` 汇总显示；`previous_dma_us` 保留在设备诊断中。

## UDP 发送与控制重连

UDP 模式把完整 `CDP1` 按设备协商的 `max_payload` 拆成 `CDU1`。客户端将 UDP socket 绑定到 WebSocket TCP 连接使用的本机 IPv4，设备据此校验来源地址。

设备声明 `udp.fec.scheme=xor_interleaved` 后，客户端按交错分组为每 8 个数据分片追加一个 XOR 校验分片。连续丢包会分散到不同组，每组最多缺少一个数据分片时设备可以恢复。旧固件不声明 FEC 时不发送校验分片，并将完整帧限制在 64 个数据分片内。

新固件通过 `hello_ack.udp.flow_control=latest_frame_ack` 声明闭环能力。JPEG 分片仍走 UDP，`udp_feedback` 走原 TCP/WebSocket 控制连接。客户端同时最多保留一个在途帧，收到匹配 sequence 的 `submitted`、`unrecoverable`、`submit_failed` 或等待 80～250 ms 自适应期限到期后，才发送当时最新画面；旧帧不重传，迟到和重复反馈不打开发送门。

分片以最多两个报文的小突发发送，pacing 初始 8 Mbit/s、范围 6～20 Mbit/s；每个突发等待到本帧累计字节对应的绝对线速时间点，不累加逐次休眠误差。单帧硬发送预算为 240 ms，编码前按当前速率、`CDU1` 头和 FEC 开销预留 20 ms 调度余量；P4 在 6 Mbit/s、`max_payload=1200`、FEC 开启时，有效数据分片预算会从 128 自动降至 118。反馈失败时速率降低 25%、FPS 降低 20%，最低 5 FPS；只有 5 FPS 下连续 3 次失败才继续收紧自适应帧预算。稳定 2 秒逐步恢复速率、稳定 5 秒恢复 FPS、稳定 10 秒恢复帧预算。旧固件没有 `flow_control` 时继续使用原累计反馈兼容逻辑。

单帧反馈失败或本地 pacing 超过 240 ms 只触发丢帧和媒体参数调整，不重建健康的 TCP 控制连接。只有 WebSocket 心跳失败、连接关闭、UDP socket 发送错误，或设备明确报告 `stream_stopped(reason="frame_timeout")` 时才重新协商会话；事件日志中的 `Reconnecting control channel after ...` 会保留本次重连的原始原因。未提供新流控能力的旧固件仍以 `submitted_frames` 判断媒体存活并保留原兼容恢复路径。

WebSocket 控制连接保持非阻塞：

- 每 2 秒发送一次 JSON `ping`；
- 连续 3 个周期未确认 `pong` 判定控制通道失效；
- 暂停 UDP 发送并按 1、2、4、5、5 秒延迟最多重连 5 次；
- 重连后重新执行 `hello`、`start_stream`，使用新的 UDP session 参数继续发送最新画面；视频目标和触摸描述必须与原会话一致，否则结束当前流并要求重新启动。
- 新流控模式下单帧超时只放弃该帧并发送最新画面，不自动切换 TCP，也不重连控制通道。

设备允许同一 PC IPv4 的新 WebSocket 替换遗留会话，因此异常断线后通常不需要手工重启设备。

## 触摸反向控制

`Stream` 面板的“触摸控制”开关（默认关闭，持久化保存）决定客户端是否在 `hello` 中订阅设备触摸事件。设备接受订阅后（`hello_ack.capabilities.touch` 为 `true`），流启动期间在设备屏幕上的触摸会转化为 PC 上的鼠标操作：

| 手势 | 注入动作 |
| --- | --- |
| 单击（按下抬起 <500 ms，位移未超过移动阈值） | 左键单击 |
| 按住移动（位移超过移动阈值） | 按住左键拖动 |
| 长按约 500 ms 不动 | 右键单击 |
| 从屏幕右缘滚动区起始的竖向滑动 | 滚轮（自然滚动方向） |

坐标链路：设备按 `hello_ack.touch.width/height` 上报坐标 → 客户端归一化到视频物理坐标 → 按显示方向反旋转 → 去除黑边并按缩放比例还原 → 叠加区域投屏选区偏移 → 平台注入。落在黑边中的触摸被忽略。移动阈值、右缘滚动区和滚动步长按设备尺寸缩放；S3 `368x448` 基准分别为 8、28、24 px，P4 `1024x600` 分别为 13、78、32 px。

平台差异：

- **Linux**：开启触摸后，portal 会话从纯 ScreenCast 变为 RemoteDesktop+ScreenCast 合并会话，系统弹窗从“共享屏幕”变为“允许远程控制”。两种会话的 restore token 不通用，切换开关会自动清除已保存的 portal 选择，下次启动需重新授权。注入坐标使用流内逻辑坐标，无需全局屏幕坐标；窗口源存在透明边裁剪时，注入位置可能有偏移。
- **macOS**：首次注入需要在 系统设置 → 隐私与安全性 → 辅助功能 中授权客户端；未授权时注入失败但视频继续。
- **Windows**：绝对坐标基于虚拟桌面；多显示器负坐标与非 100% DPI 场景请以实测为准。

注入后端不可用（portal 被拒、权限缺失）时，客户端仅在终端日志打印动作（`touch inject (log)`），视频不受影响。`Static color bars` 测试源始终只打印日志，适合核对坐标映射。设备侧是否在推送可通过 `Device` 面板的“触摸”行或 `/api/status` 的 `touch_events_active` 判断。

## 指标与前端判定

指标含义：

| 指标 | 含义 |
| --- | --- |
| `Capture` | `处理/等待 ms`；Linux 的处理值来自 PipeWire 裁剪缩放，等待值来自发送线程取得下一帧；界面悬浮提示显示两者顺序 |
| `Encode` | 变化检测、锐化、JPEG 编码和帧封装耗时；`JPEG` 后的 `Nx` 是本帧编码尝试次数 |
| `Send` | TCP 写入或 UDP 分片发送耗时；UDP 包含主动 pacing 等待 |
| `UDP` | 当前数据分片预算 `NF`、累计 FEC 恢复 `+N` 和不可恢复帧 `-N`；仅 UDP 模式显示 |
| `UDP LINK` | 当前 UDP pacing 速率和经 TCP 逐帧反馈测得的交付时间 EWMA |
| `Scale` | 当前有效来源缩放到设备内容区域的百分比；数值越小表示缩小越强 |
| `JPEG` | 实际使用的质量，以及当前 JPEG 字节数与协商上限 |
| `Device` | 设备报告的排队、解码和显示提交耗时 |
| `Replaced` | 设备帧管线主动替换的待处理旧帧数 |
| `Dropped` | PC 采集、图像处理、编码或发送路径累计丢帧数 |

性能阈值为当前帧预算的 90%：

```text
threshold_ms = floor(1000 / fps * 0.9)
```

例如 20 FPS 为 45 ms，25 FPS 为 36 ms。前端分别保留最近 5 次采集、编码、发送和设备指标并取窗口最大值；任一阶段超阈值时对应指标变红，预览显示 `Performance limited`。

## 性能与排障

| 现象 | 判断与处理 |
| --- | --- |
| 设备探测失败 | 确认 PC 与设备同网段、地址指向 `/stream` 所在主机和端口 |
| Pairing code rejected | 重新输入设备屏幕当前六位配对码 |
| Device busy | 在另一客户端 Stop；异常残留可等待同 IPv4 重连替换或重启设备 |
| TCP 画面慢 | 查看 `Device`；TCP 本身不会积压多帧，瓶颈通常在采集、编码或设备绘制 |
| UDP `rx` 不增长 | 检查端口、防火墙、路由和 PC UDP socket 的本机 IPv4 |
| `accepted` 增长、`completed` 不增长 | 网络丢分片超过 FEC 恢复能力，查看 `FEC +恢复/-失败` 和 `UDP LINK`；客户端先降低 pacing 与 FPS |
| `completed` 增长、`submitted` 不增长 | `CDP1` 长度、CRC、尺寸或帧管线提交失败；查看 `lastOutcome=submit_failed`，单帧失败不会断开控制连接 |
| `Capture` 很高 | 停止并重新建立 Portal 共享；超过 500 ms 优先检查采集端 |
| `Encode` 很高 | 查看 `JPEG` 的编码次数；变化帧通常为 `1x`，静态缓存重放不显示次数，持续多次表示内容在帧上限附近波动 |
| `Send` 很高 | UDP 大帧需要分片 pacing；需要画质时改用 TCP，需要低延迟时降低质量或 FPS |
| UDP 反复显示 `STREAM TIMEOUT` / 重连 | 查看 `Reconnecting control channel after ...` 的原因；若只有 `UDP frame exceeded the 240 ms send budget`，确认使用包含动态线速分片预算的新客户端 |
| Linux CPU 超过 100% | 先用 `npm run tauri:dev:release` 排除调试构建开销，再查看终端 `PipeWire capture stats` 的回调率、转换率和耗时 |
| `Scale` 很低、细字偏软 | 使用区域投屏或提高来源界面缩放，减少整屏强缩小 |
| `Device` 很高 | 降低 FPS/质量；检查设备 JPEG 解码与显示后端指标 |
| 窗口带大块黑色背景 | 检查是否存在 `PipeWire VideoCrop`；无 crop 和 alpha 时会回退完整帧 |
| 客户端窗口固定色块 | 确认当前版本已设置 `WEBKIT_DISABLE_DMABUF_RENDERER=1` |
| Stop 后重新 Start 首帧失败 | 过渡帧会自动跳过；持续 10 秒无有效帧时重新选择共享对象 |
| Portal 已选中但设备无画面，`lastReject=inactive` | 确认客户端日志中先出现 `First frame is ready` 再启动设备流；旧客户端会在首帧准备前启动设备并触发无帧超时 |

推荐排障顺序：TCP + 色条验证基础链路，随后 Portal + TCP 验证采集，最后切换 UDP 检查低延迟与分片统计。

## 安全与兼容性

客户端仅支持控制协议 v3，并会拒绝不匹配设备。配对码和 UDP session 参数均为明文，WebSocket 使用 `ws://`，UDP 不加密；只应在可信局域网使用。触摸控制会向操作系统注入真实输入，默认关闭，且受各平台注入权限约束。

新增文字或音频能力时，必须先更新 [`PROTOCOL.md`](../PROTOCOL.md)，再同步实现设备端与 PC 端的鉴权、生命周期、失败处理和兼容策略。
