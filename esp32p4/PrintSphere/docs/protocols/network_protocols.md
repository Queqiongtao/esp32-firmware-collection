# 网络与应用协议

## ESP32-P4 到 ESP32-C6

P4 通过 SDIO Slot 1（4-bit、25 MHz、TX/RX 队列各 20）运行 ESP-Hosted 2.12.8，C6 提供 Wi-Fi 数据面。BSP 在 `esp_hosted_connect_to_slave()` 前注册 Hosted 事件，并等待最多 5 秒的 `TRANSPORT_UP`；`TRANSPORT_FAILURE`、`TRANSPORT_DOWN` 或超时会阻止后续 Wi-Fi 初始化。失败日志记录等待耗时、最后事件、是否收到 CP_INIT 和 transport ready 状态。本地 MQTT 将 TCP 建连超时与 TLS 证书错误分开记录；连续失败按 1、2、4、8 次幂频率输出 STA IPv4、掩码、网关和目标端点，且重连看门狗晚于 MQTT 自身的 20 秒网络超时，避免同一次失败重复调度客户端重建。

## 配网与 Web Config

- Setup AP：`PrintSphere-Setup`，默认地址 `http://192.168.4.1`
- USB 串行配网：沿用上游 SerialProvisioner 帧协议
- Web Config：ESP-IDF `esp_http_server`，HTTP JSON API
- 安全：局域网访问可启用临时六位 PIN；凭据保存在 NVS，不写入日志或文档

主要 API 包括 `/api/health`、`/api/unlock`、`/api/wifi/scan`、`/api/config`、`/api/printers/*`、`/api/cloud/*`、`/api/local/connect`、`/api/audio/*`、`/api/ota/*` 和 `/api/debug/log`。改变显示方向仅接受 `0` 或 `180`。

## Bambu Cloud

- HTTPS：账户登录、2FA/验证码、绑定列表、偏好、任务和封面下载
- MQTT/TLS：区域 broker 的 `8883` 端口，MQTT 3.1.1
- Topic：`device/<serial>/report` 与 `device/<serial>/request`
- 认证：Cloud access token；Cloud HTTPS 和 MQTT/TLS 使用 ESP-IDF 公共 CA bundle，并启用交叉签名证书链验证，以兼容服务端返回的 GlobalSign 等交叉签名链
- 限制：Cloud API 属于外部服务，字段和认证流程可能变化，需保留失败退避

## 本地打印机

- MQTT/TLS：打印机局域网接口，用户名默认 `bblp`，密码为 access code
- 证书：固件内嵌的 Bambu CA bundle 仅用于局域网打印机 MQTT/TLS，不作为 Bambu Cloud HTTPS 的公共信任根
- Topic：`device/<serial>/report` 与 `device/<serial>/request`
- 数据：JSON 状态、`pushall`/`get_version` 请求、灯光及可选打印控制命令
- 相机：支持的 A1/P1 系列通过 TLS 端口 `6000`，80 字节认证包，16 字节帧头，单帧上限 256 KiB；长连接持续取帧并按 `PRINTSPHERE_CAMERA_TARGET_FPS`（默认 5）目标帧率自适应节流，手动刷新跳过节流立即取帧；读取 JPEG 源尺寸后等比解码到不超过 1024x576 的 8 像素对齐尺寸，小分辨率源不放大，失败时降级到 800x448 或 400x224，并缓存上次成功解码尺寸避免每帧重复探测；相机推流快于目标帧率时 TCP 缓冲会积压约 1~2 帧（延迟约 100~200 ms），属预期边界
- RTSP：本工程不实现，因此 H2/X1/X2 等 RTSP 机型无实时相机页

## 状态融合

`cloud_only` 只使用 Cloud；`local_only` 只使用 LAN；`hybrid` 依据连接状态和时间戳选择更可靠的新鲜值。本地相机只在局域网路径可用。所有外部 payload 必须受最大长度、超时和 JSON 解析失败保护。

## OTA

Web Config 支持上传 app 镜像或从 HTTPS URL 拉取。两条路径都校验 ESP 镜像格式、目标芯片和 app descriptor 中的 `project_name=printsphere_p4`，通过后才切换非活动 OTA 槽。bootloader rollback 已启用；新镜像只有在关键启动流程和 UI 初始化完成后才标记有效。JC1060P470 包必须使用独立硬件后缀，禁止与 S3 或其他 P4 工程镜像混用。正式发布前仍必须真机验证错误镜像拒绝、断电恢复、自动回滚以及 NVS/LittleFS 保留。
