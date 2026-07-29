# Companion Display ESP32-P4 固件

本工程将 JC1060P470 接入 Companion Display v3。设备使用 ESP32-P4 V1.x、JD9165 1024x600 显示屏和 GT911 触摸控制器；ESP32-P4 本身没有 Wi-Fi，网络由板载 ESP32-C6 通过 ESP-Hosted SDIO 提供。控制协议、公共帧管线和 PC 客户端与 S3 固件共用。

## 工程结构与数据流

| 模块 | 职责 |
| --- | --- |
| `main` | 初始化显示和帧管线，循环等待 C6 Hosted transport，上线后启动公共网络服务 |
| `components/companion_board` | JD9165、GT911、共享 I2C、背光和 ESP-Hosted 板级初始化 |
| `components/subscreen_display` | 状态界面、硬件 JPEG 解码、PPA 复制、DPI framebuffer 提交 |
| `components/esp_lcd_jd9165` | JC1060P470 使用的 JD9165 初始化序列和面板驱动 |
| `../common/components` | S3/P4 共用的帧校验、最新帧队列、触摸事件化、配网、HTTP、WebSocket 和 UDP 重组 |

```text
PC JPEG -> CDP1 / CDU1 -> ESP32-C6 Wi-Fi -> SDIO -> ESP32-P4
                                                        |
                                        校验 -> 硬件 JPEG 1024x608
                                                        |
                                        PPA 裁取 1024x600 RGB565
                                                        |
                                  空闲 DPI framebuffer -> 刷新边界换帧

GT911 800x480 -> 板级缩放 1024x600 -> touch_events -> PC 鼠标注入
```

## 硬件与默认配置

| 项目 | 当前默认值 |
| --- | --- |
| 芯片与存储 | ESP32-P4 rev 1.00～1.99；16 MiB Flash；片上 HEX PSRAM 200 MHz |
| 分区 | NVS `0x6000`、PHY `0x1000`、4 MiB factory app；当前没有 OTA app 分区 |
| 显示 | JD9165、1024x600、RGB565、2 条 DSI data lane、每 lane 600 Mbps、DPI 54 MHz |
| DSI 电源与控制 | MIPI DPHY LDO channel 3 / 2.5 V；LCD reset GPIO27 |
| 背光 | GPIO23、20 kHz PWM、默认 80% |
| 触摸 | GT911；I2C1 GPIO7/8、400 kHz；RST GPIO22；INT GPIO21 |
| Hosted SDIO | Slot 1、4-bit、25 MHz；TX/RX queue 各 20；D0～D3 GPIO14～17、CLK GPIO18、CMD GPIO19 |
| C6 复位 | GPIO54、active-high；P4 每次启动复位 C6，默认延迟 1500 ms |
| 网络服务 | HTTP/WebSocket 80；UDP 5000；UDP 接收邮箱 64 包 |

`CONFIG_COMPANION_DISPLAY_WIDTH/HEIGHT` 必须保持 `1024x600`，帧管线启动时会与显示后端交叉校验。当前构建选择 P4 `<3.0` 寄存器路径，不应直接用于 rev 3.0 及以上芯片。

默认 TCP 完整帧上限为 `393240` 字节，即 384 KiB JPEG 加 24 字节 `CDP1` 头。UDP 最多使用 128 个、每个 1200 字节的数据分片，因此协商的有效完整帧上限仍为 `153600` 字节，JPEG 上限为 `153576` 字节。只有 JPEG 分片走 UDP，逐帧结果经 TCP/WebSocket 返回；PC 同时只保留一帧在途。UDP 另行协商交错 XOR FEC，每 8 个数据分片增加一个校验分片，可恢复每组一个丢失分片；重组和 FEC 累加缓冲均从 PSRAM 分配。输入 JPEG 头声明的尺寸必须严格为 `1024x600`。

## 前置条件

- 安装 `eim`，并确保它可以选择 ESP-IDF 6.0.1；`dev.sh` 不依赖调用者预先 source `export.sh`。
- 首次构建会根据 `idf_component.yml` 解析托管组件，并生成被仓库忽略的本地 `dependencies.lock`。当前已验证的本地解析结果为 `esp_hosted 2.12.8`、`esp_wifi_remote 1.6.0`、`esp_lcd_touch_gt911 1.2.0~3`；后续构建以实际生成的 lock 为准。
- 板载 C6 必须运行与 Host 端相同 ESP-Hosted 版本/代码基线的 slave 固件，并使用 SDIO 4-bit、25 MHz 和 streaming mode。不能假定任意出厂 slave 固件均与当前 Host 兼容。

## 构建、烧录与清理

在本目录执行：

```bash
./dev.sh build
./dev.sh ports
./dev.sh -p /dev/ttyACM0 flash-monitor
```

默认串口为 `/dev/ttyACM0`，可用 `-p PORT` 或 `PORT=` 覆盖。JC1060P470 应使用板上丝印 `FULL USB` 的 USB Serial/JTAG 口烧录和查看日志；`HIGH USB` 是应用侧 DWC OTG 口，不要混用。串口实际名称以 `./dev.sh ports` 为准，monitor 使用 `Ctrl-]` 退出。

其他常用命令：

```bash
./dev.sh menuconfig
./dev.sh clean
./dev.sh fullclean
./dev.sh idf --version
./dev.sh idf -p /dev/ttyACM0 erase-flash
```

构建日志默认写入 `/tmp/eim-companion_display_p4.log`，可用 `EIM_LOG_FILE` 覆盖。`clean`、`fullclean` 只清理本机构建输出，不会清除设备 NVS；普通重新烧录也会保留 Wi-Fi 凭据和配对码。需要恢复首次配网状态时，执行 `erase-flash` 后重新烧录。

默认 DSI 速率是每 lane 600 Mbps。如果特定面板批次冷启动不稳定，可在 `Companion Board (JC1060P470) -> MIPI DSI lane bitrate` 中人工改为 750 Mbps。固件没有运行时自动回退机制。

## ESP32-C6 Slave 固件

P4 的 `./dev.sh flash` 只烧录 ESP32-P4，不会更新板载 C6。当前 Companion 固件也没有集成 ESP-Hosted slave OTA 分区或更新流程。

需要更新 C6 时，先构建一次 P4 工程，再以本地 `dependencies.lock` 中的 Host 版本创建完全相同版本的 slave 工程。当前已验证版本的示例：

```bash
idf.py create-project-from-example "espressif/esp_hosted=2.12.8:slave"
cd slave
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p C6_SERIAL_PORT flash monitor
```

在 menuconfig 中确认 transport 为 SDIO、4-bit、25 MHz，并且 Host 与 slave 同为 streaming mode。C6 的下载口、boot strap 和外部编程器接线必须以 JC1060P470 板卡资料为准，不要把 P4 的 `FULL USB` 串口当作 C6 烧录口。版本不一致常表现为 RPC timeout、兼容性告警或 transport 无法上线。

## 启动、配网与使用

启动顺序是显示和帧管线、ESP-Hosted、Wi-Fi/HTTP/UDP、触摸轮询。显示、JPEG 或 PPA 初始化失败属于启动致命错误；C6 缺失时设备保留状态画面并重试。每次 Hosted 连接最多等待 15 秒，失败后再退避 2 秒，因此并非固定每 2 秒完成一次连接尝试。C6 未上线前 SoftAP、HTTP 和触摸任务尚未启动。

正常启动会出现以下串口里程碑；`control ready` 与异步触摸任务产生的 `touch ready` 可能前后交错：

```text
display ready: JD9165 ...
ready: physical=1024x600 hardware JPEG + PPA ...
ESP-Hosted transport up
control ready; UDP port=5000
touch ready: GT911 ...
```

首次使用：

1. 连接 `Companion-XXXX` 热点，后缀取 AP MAC 最后两字节；默认密码为 `companion-display`。
2. 打开 `http://192.168.4.1/`，填写目标 2.4 GHz Wi-Fi 的 SSID 和密码。
3. 设备获得 DHCP 地址后，屏幕显示 IPv4 和六位配对码。当前没有 mDNS，PC 客户端需要直接填写 IP。
4. PC 客户端先用 TCP + `Static color bars` 验证尺寸、颜色和显示，再测试 UDP 与实际采集。

设备始终运行 APSTA：联网后配网热点不会关闭，默认 WPA2、信道 1、最多两个客户端。目标 Wi-Fi 凭据和六位配对码保存在 P4 的 `companion` NVS namespace，目前没有单独的“忘记网络”或“重置配对码”接口。配网页和视频协议均为明文，只适合可信局域网；产品部署应修改默认热点密码。

## 触摸行为

GT911 按 `0x5D`、`0x14` 的顺序探测，只上报第一个触点。控制器原生坐标默认是 `800x480`，板级代码按比例缩放为 `1024x600`；设备不交换坐标轴、不镜像，也不处理显示方向，旋转和黑边反映射由 PC 客户端完成。

触摸任务每 10 ms 轮询，移动事件约每 15 ms 合并，协议声明 `max_move_hz=60`。初始化失败后每 500 ms 再尝试，视频链路仍可使用。只有 GT911 已就绪且本次 `hello` 显式订阅触摸时，`hello_ack.capabilities.touch` 才为 `true` 并携带 `touch` 描述；若客户端过早协商，可等待 `/api/status.touch=true` 后重新连接。

## 诊断与排障

设备屏幕会显示 `WS CONNECTED`、`NEGOTIATING`、`TCP VIDEO`、`UDP VIDEO`、`BAD SIZE`、`BAD CRC`、`NO BUFFER`、`UDP REJECTED`、`SESSION EXPIRED` 和 `STREAM TIMEOUT` 等状态。`GET /api/status` 提供视频、触摸、UDP 和最近一帧性能诊断。

P4 上的性能字段含义：

| 字段 | 含义 |
| --- | --- |
| `jpegDecodeUs` | 硬件 JPEG 解码耗时；单次硬件等待上限为 200 ms |
| `lcdWaitUs` | 等待上一 framebuffer 达到可安全复用状态的耗时 |
| `lcdSubmitUs` | PPA 复制有效区域并提交 framebuffer 的耗时 |
| `previousDmaUs` | 兼容字段名；上一帧从提交到刷新完成的耗时 |
| `replacedFrames` | 尚未开始绘制的旧帧被新帧替换的累计数量 |

常见问题：

- Hosted 持续 timeout/failure：确认 C6 slave 与 P4 本地 `dependencies.lock` 中的 `esp_hosted` 版本匹配（当前已验证为 `2.12.8`），并检查 SDIO Slot 1、4-bit、streaming mode、25 MHz、GPIO14～19 和 C6 reset GPIO54。
- Hosted 已启动后 transport down：当前依赖 ESP-Hosted 内部 restart 和 Wi-Fi 事件处理；完整运行时恢复尚待上板验证，必要时重启设备并保留完整串口日志。
- 看不到 SoftAP：先确认日志已到 `ESP-Hosted transport up`；C6 未上线时网络服务尚未初始化。
- 显示无画面或启动复位：检查 `display ready`、PSRAM、硬件 JPEG 和 PPA 初始化日志。`NO BUFFER` 通常表示 PSRAM 或帧缓冲不可用。
- `JPEG header rejected` / `BAD SIZE`：客户端必须按 `hello_ack` 生成实际 `1024x600` JPEG，不能只修改 `CDP1` 帧头尺寸。
- `JPEG decode failed`：检查 JPEG 是否完整、编码格式是否受硬件支持，并确认没有超过 200 ms 解码等待。
- 红蓝互换：硬件 JPEG 路径必须使用 `JPEG_DEC_RGB_ELEMENT_ORDER_BGR`，不要按枚举名称改回 RGB。
- 画面撕裂：当前路径只向空闲 framebuffer 写入，等待 `on_refresh_done` 后复用旧帧；不要改回 DPI 内置 DMA2D 的外部缓冲复制路径。
- UDP `rxPackets` 增长而 `completedFrames` 不增长：查看 `fecRecoveredFragments`、`fecUnrecoverableFrames`、`lastOutcome` 和 `lastReassemblyUs`；PC 会先降低 pacing 与实际 FPS，达到 5 FPS 且持续失败后才降低帧预算。`lastReject` 仍用于区分 session、来源、头和长度错误。
- 桌面细字偏软：PC 端优先使用 TCP、20 FPS、质量 85，并观察 `Scale` 与 `JPEG` 指标；`Scale` 很低时使用区域投屏减少强缩小。
- GT911 不响应：检查 I2C GPIO7/8、RST GPIO22、INT GPIO21 和两个地址的探测日志。边缘比例异常时调整 GT911 native `800x480` 配置，不要修改协议显示尺寸。

## 验证状态

当前版本已完成 P4 `fullclean` 后的完整构建验证，但本轮尚未完成 Companion 固件硬件验收。源 JC1060P470 综合测试工程的历史结果不能替代本固件验收。

上板至少覆盖：

- 600 Mbps 和人工 750 Mbps 两种每 lane 速率的多次冷启动；
- 色条、JPEG 色序、连续全帧刷新、撕裂和残影；
- GT911 四角、边缘、按下/抬起以及顶部朝上/左/下/右映射；
- C6 缺失时的 15 秒等待加 2 秒退避，以及匹配 slave 上线；
- SoftAP、STA、APSTA 常驻行为、NVS 持久化和擦除恢复；
- TCP/UDP、最大帧、丢分片、静态 1 FPS 保活和会话超时；
- 启动完成后的 Hosted 断链与恢复行为。
