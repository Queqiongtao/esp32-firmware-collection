# 测试与验收计划

## 当前证据

2026-07-29 使用 `./dev.sh build`，工具确认 ESP-IDF `v6.0.1`，完成 rollback bootloader、应用链接和镜像生成。应用镜像 `0x330300` 字节，最小 5 MiB app 槽余量约 36%。`esptool image-info` 确认目标为 ESP32-P4 V1.0、项目名为 `printsphere_p4`。`./dev.sh idf release_initial_flash` 同日成功生成以下本地构建产物：

- 初始整包：4,457,216 字节，SHA-256 `052519760a51e110a25f79508feea2689ee3984e9d6adf48e40e03c896616f7b`
- OTA 包：3,343,104 字节，SHA-256 `120eff451370b0e8773b3498bff5280ab6db5ce617a45211997180e17bcb06b1`

这些结果只证明编译、链接、静态分区容量和打包流程通过；产物位于被忽略的 `release/`，尚无本工程真机证据，不属于可发布版本。

2026-07-30 针对 Bambu 中国区 Cloud 登录在 TLS 握手阶段出现 `No matching trusted root certificate found` 和 `mbedtls_ssl_handshake returned -0x3000` 完成修复。原工程与迁移工程内嵌的 4 个 Bambu 局域网 CA 文件及其 SHA-256 一致；Cloud 请求实际使用 ESP-IDF 公共 CA bundle。`api.bambulab.cn` 当日返回 `GlobalSign GCC R3 DV TLS CA 2020` 到 `GlobalSign Root CA - R3` 的交叉签名链，因此启用 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`，未启用已弃用根证书或跳过 Cloud 证书校验。使用 ESP-IDF `v6.0.1` 执行 `./dev.sh build` 通过，生成配置同时启用 `CONFIG_MBEDTLS_X509_TRUSTED_CERT_CALLBACK=y`；应用镜像为 `0x330190` 字节，最小 5 MiB app 槽剩余 `0x1cfe70` 字节（36%）。本次仅完成编译验证，尚未烧录并复测 Cloud 登录，真机结论保持待验证。

2026-07-30 针对 P1 系列相机清晰度，将固定 `400x224` 解码改为读取 JPEG 源尺寸后等比解码到最大 `1024x576`，并保留 `800x448`、`400x224` 两级失败降级；相机页同步改为 `1024x576` 沉浸式画面及上下半透明信息条。使用 ESP-IDF `v6.0.1` 执行 `./dev.sh build` 通过，应用镜像为 `0x330870` 字节，最小 5 MiB app 槽剩余 `0x1cf790` 字节（36%）。通过 `/dev/ttyUSB0` 烧录至 ESP32-P4 revision v1.0（MAC `60:55:f9:fb:0b:18`），写入数据 hash 校验通过并完成硬复位。复位后 60 秒串口观察未发现崩溃、看门狗或内存分配失败；可用 PSRAM 约 28.5 MiB，但打印机本地 MQTT `192.168.51.83:8883` 持续出现 `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`，现有相机门控未进入端口 6000 的取帧路径，因此尚未取得 `Camera JPEG decoded` 的源尺寸、输出尺寸、耗时和降级次数日志。实际画质、上下信息条、连续多帧稳定性及 0°/180°显示效果保持待验证。

2026-07-31 针对本地 MQTT 在 TCP `connect()` 阶段持续出现 `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`，确认同一 Wi-Fi 内开发机可与 `192.168.51.83:8883` 完成 TLS 1.2 握手，打印机证书 CN 为 `01P00C532100111`，故障不属于 CA、access code 或打印机端口关闭。将迁移工程的 ESP-Hosted SDIO TX/RX 队列由 `6/6` 恢复为组件默认及同板 P4 工程使用的 `20/20`，并将断连看门狗延后至 MQTT 20 秒网络超时之后，避免同一次失败由看门狗和 `MQTT_EVENT_ERROR` 重复调度重建；TCP 超时诊断按失败次数 1、2、4、8 次幂限频记录 STA IPv4、掩码、网关和目标。使用 ESP-IDF `v6.0.1` 执行 `./dev.sh build` 通过，生成配置确认 `CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=20`、`CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=20`；应用镜像为 `0x330a30` 字节，最小 5 MiB app 槽剩余 `0x1cf5d0` 字节（36%）。验证时 `/dev/ttyUSB0`、`/dev/ttyACM*` 及 `/dev/serial/by-id/*` 均未枚举，尚未完成本次固件烧录、本地 MQTT 订阅和相机连续帧复测，真机结论保持待验证。

2026-07-31 针对相机预览流畅度，将 1.5 秒定时取帧改为长连接持续取帧：目标帧率由新配置 `CONFIG_PRINTSPHERE_CAMERA_TARGET_FPS`（默认 5fps，范围 1~10）控制并按“目标间隔 − 本帧耗时”自适应节流；解码缓存上次成功源/输出尺寸，跳过每帧重复的 JPEG 源尺寸探测；相机页激活时主循环合并周期从 500ms 降到 200ms；新增每 30 帧一次的 `Camera stats` 日志（实际帧率、平均取帧耗时、降级次数），手动刷新仍跳过节流立即取帧。使用 ESP-IDF `v6.0.1` 执行 `./dev.sh idf reconfigure` 后 `./dev.sh build` 通过，确认 `CONFIG_PRINTSPHERE_CAMERA_TARGET_FPS=5`；应用镜像为 `0x330f60` 字节，最小 5 MiB app 槽剩余 `0x1cf0a0` 字节（36%）。本次仅完成编译验证，尚未烧录真机；相机实际帧率、端到端延迟、PSRAM 余量、30 分钟连续取帧稳定性及 0°/180° 画面效果保持待验证。

## 构建门禁

1. 在工程根执行 `./dev.sh build`，无 warning-as-error 或链接错误。
2. 核对日志中的 IDF 版本必须是 `v6.0.1`。
3. 核对 target 是 ESP32-P4、Flash 16 MiB、两个 app 槽均为 5 MiB。
4. 执行发布打包时记录初始包和 OTA 包 SHA-256。

## 首次上板

1. 擦除测试板后烧录，保存从 ROM 到 UI ready 的完整串口日志。
2. 确认 JD9165 无黑屏、白条、闪烁、颜色错误或撕裂。
3. 在 0°执行四角、四边中心和屏幕中心九点触摸；180°重复。
4. 确认 Hosted 在 5 秒内进入 transport ready，随后能扫描和连接 AP。
5. 连续执行 10 次软重启和 10 次断电冷启动，不得卡在 Hosted、I2C 或 LVGL 初始化。
6. 串口确认 LVGL task 运行在配置的 core 1；0°/180°触点只经过 LVGL 一次旋转。

## 功能矩阵

| 能力 | 核心步骤 | 通过标准 |
| --- | --- | --- |
| 配网 | USB 串行、Setup AP、家庭 Wi-Fi | 三路径可配置，重启后自动重连 |
| Web Config | PIN、锁定、所有设置页 | 未授权被拒，设置持久化 |
| Cloud | 公共 CA/交叉签名 TLS、登录、验证码/2FA、绑定、封面 | TLS 握手无 `-0x3000`；状态持续更新，失败有退避 |
| Local | MQTT TLS、pushall、断线重连 | 状态和命令闭环，无任务泄漏 |
| Hybrid | Cloud/LAN 切换和断网 | 数据源切换无明显回退或假在线 |
| AMS | 0/1/多 AMS、外置料盘、错误 | 四槽无裁切，状态对应正确 |
| Camera | 端口 6000、自动刷新、弱网 | 图像比例正确，失败后恢复 |
| Audio | 内置音、音量、自定义 WAV | ES8311/PA 正常，无爆音和阻塞 |
| Battery | 无电池、仅电池、电池+USB | 电压合理；外部供电未知时不误启用纯电池关屏 |
| OTA | 上传、URL、A/B、错误工程、断电 | 错误镜像不切槽；关键启动失败回滚；NVS 和声音保留 |

## 1024x600 UI 检查

打印机列表、每个 AMS 页、主仪表、封面、相机、亮度提示和 Web PIN 覆盖层都必须在最长预期文本下检查。记录 0°/180°截图，确认无重叠、裁切、溢出、错误缩放和滑页跳页；状态环和进度文字只能出现在主仪表页。启用实验打印控制后，确认暂停/停止按钮位于封面右侧且不遮挡文本。相机页核对日志中的 JPEG 源尺寸和自适应输出尺寸；P1 系列 16:9 源应输出 1024x576，画面不得拉伸，顶部状态和底部层数/任务名叠层不得溢出或遮挡彼此。

## 稳定性与故障注入

- 8 小时打印监控并周期切换页面，观察 heap、最大连续块、看门狗和网络重连。
- 启动时让 C6 不响应，确认 5 秒超时和明确日志；恢复硬件后冷启动可恢复。
- Wi-Fi 弱信号、AP 重启、DNS 失败、Cloud 认证失败、本地打印机关机均不得导致 UI 卡死。
- 上传损坏 OTA、S3 镜像、`project_name` 非 `printsphere_p4` 的 P4 镜像和超大声音文件，必须拒绝且保持当前固件可启动。
- 从 pending verify 镜像主动制造关键启动失败，确认 bootloader 自动回到上一槽；正常启动到 UI ready 后确认镜像不再回滚。

## 记录模板

每次真机验证记录：日期、测试人、Git 提交、`PROJECT_VER`、板卡修订、C6 固件信息、供电方式、串口设备、测试步骤、关键日志、截图/视频路径、结论和遗留问题。没有这些证据的项目保持“待上板”状态。
