# 验证状态与实验记录

## 记录规则

本文只记录可追溯的验证证据。状态含义如下：

- **静态检查**：根据当前源码、配置和依赖接口得出的结论。
- **编译验证**：指定配置完成编译链接，不代表硬件功能正常。
- **上板验证**：记录了实际固件、硬件路径和可观察结果。
- **待验证**：实现存在，但没有足够证据声明当前版本通过。

旧提交的结果只证明当时路径，不自动继承到修改后的实现。

## 当前基线

| 项目 | 当前值 |
|------|--------|
| 芯片 | ESP32-P4 V1.0，`CONFIG_ESP32P4_REV_MIN_FULL=100` |
| ESP-IDF | 6.0.1 |
| 面板 | JD9165，1024x600 RGB565，2-lane DSI |
| 显示参数 | `num_fbs=2`，lane rate 750 Mbps，DPI clock 54 MHz expect |
| LVGL | 9.4.0，esp_lvgl_adapter 0.6.2，DOUBLE_DIRECT |
| HIGH USB 默认能力 | UVC Host，运行时初始角色 None |
| UVC 默认分辨率 | 稳定偏好 1280x720，UI 可手动选择其他枚举档位 |

## 当前运行记录

### 2026-07-18：640x360 UVC 低分辨率路径

- 证据来源：FULL USB `/dev/ttyUSB0` 只读日志；未重新烧录。
- 当前仓库 HEAD：`1fda5b5`。
- UVC：`640x360`，PPA backend，streaming。
- 5 秒窗口：`ingress=125 received=125 decoded=125 published=125`。
- 同一窗口：`errors=0 truncated=0 skipped=0 dropped=0 transfer=0 overflow=0 underflow=0`。
- JPEG decode 约 3.2~3.3 ms，PPA preview 约 12~15 ms。
- LVGL 日志约 22~23 fps，refresh 约 23~24 ms，render 约 11 ms，flush 约 2 ms。

结论：该现有固件的 640x360 收流、硬解、PPA 发布和显示刷新有上板运行证据。该记录
不覆盖修改后的重新烧录，也不证明 720p/1080p、拔插、角色释放或自动恢复稳定。

## 既有上板记录

| 路径 | 状态 | 证据边界 |
|------|------|----------|
| JD9165 显示 + GT911 触摸 | 已上板验证 | DOUBLE_DIRECT 双 framebuffer 组合未观察到白条、花屏、闪烁、黑屏、撕裂或残影 |
| SDMMC SD 卡 | 已上板验证 | Slot 0 与 Hosted Slot 1 共存，挂载及文件读写回环通过 |
| RX8025T-UC RTC | 已上板验证 | 读取、无效时间同步和周期日志有记录 |
| ES8311 音频 | 已上板验证 | 播放、录音与回放路径有既有记录 |
| 电池 ADC | 已上板验证 | GPIO52 分压采样和 UI 展示有既有记录 |
| Hosted WiFi STA | 已上板验证 | P4-C6 SDIO transport、扫描和连接有既有记录 |
| HIGH USB CDC echo | 历史构建已上板验证 | 枚举、RX dump、原样回显和 line state 有记录 |
| UVC 能力枚举与 1080p 出流 | 已上板验证 | 曾命中 1920x1080@30fps；这是手动高负载档记录，不是当前默认值 |
| UVC PPA letterbox 预览 | 已上板验证 | 1080p PPA 缩放约 90 ms/次，画面可见 |

## UVC 实验历史

### ISOC 与损坏帧

- 4 x 20 KiB URB（约 10 ms 等时缓冲、80 KiB 内部 DMA RAM）有上板使用记录。
- 8 x 20 KiB 会使 HCD 的 ISOC qTD 调度绕死，并诱发 control timeout/断言复位，已回退。
- 某组 1080p 按需解码记录中，5 秒窗口约 `decoded=20`、`errors=3.5`，约 15% 解码尝试
  为损坏帧；应用静默丢弃后由下一帧补上。
- 迁移 USB 栈到 core 0 的单变量试验未改善错误率，已回退到当前 core 1 配置。
- 这些结果与特定摄像头、线缆和旧实现相关，不能写成所有设备的固定错误率。

### 解码和预览负载

- 旧单缓冲/限流路径：1080p JPEG 约 21~34 ms、PPA 约 84~103 ms、屏幕约 3 fps；
  640x360 JPEG 约 1.5~5.6 ms、PPA 约 15~19 ms、屏幕约 7~8 fps。
- CPU 缩放 720p 曾测得约 225~231 ms/帧，并伴随明显 underflow，不作为默认方案。
- 双缓冲早期试验中，1080p 曾出现连续 decoder 错误、`dma2d_force_end` 和 root-port 恢复；
  后续将异常 probe 的零 frame size 与真实 PSRAM 不足区分，并固定 1 MiB frame buffer。
- 当前低分辨率不放大及有效区域复制结果见“当前运行记录”。

## 当前待验证矩阵

| 路径 | 当前状态 | 验收内容 |
|------|----------|----------|
| 默认 UVC 720p | 待当前版本上板复测 | 连续运行、帧率、错误/overflow/underflow、内存稳定性 |
| 手动 1080p 与自动降级 | 待当前版本上板复测 | 错误阈值、就地切换 720p、UI 自动降级状态 |
| 全部分辨率运行时切换 | 编译路径已实现 | 成功、不支持档位、切换中拔出 |
| UVC 页面反复进入/退出 | 编译路径已实现 | UVC/None 循环、task/queue/buffer/controller 完整释放 |
| transport/worker/control timeout 恢复 | 编译路径已实现 | 故障注入、device-gone 屏障、Stack 重建和恢复耗尽 |
| UVC 热复位恢复 | 编译路径已实现 | RTC no-init 标记、自动启动及无真实 VBUS 断电边界 |
| CPU 预览回退 | 编译路径保留 | 画面正确性和长期资源稳定性；不作为性能目标 |
| Hosted BLE | 软件路径已实现 | 广播、连接、GATT read/write/notify 和重复启停 |
| WiFi/BLE 重连与释放 | 软件路径已实现 | 重连、失败回滚、deinit 后重新 init |

CDC 与 UVC 必须使用两套独立固件验收。CDC 固件覆盖 CDC/None 循环、枚举和回显；UVC
固件覆盖 UVC/None 循环、分辨率、拔插、热复位和故障恢复。两套测试期间都要确认 FULL
USB 日志口不受影响。

## 本次审查验证边界

本次审查允许读取现有板上日志，但不覆盖固件、不切换 USB 物理接线。代码和文档修改后
只以静态检查、自动化检查和多配置编译为结论；未重新烧录的项目不得标记为本次上板通过。

### 2026-07-18 多配置编译

以下配置均使用 ESP-IDF 6.0.1、ESP32-P4 target，在 `/tmp` 隔离目录完成编译链接：

| 配置 | 结果 | app 大小 | 4 MiB 分区余量 |
|------|------|----------|----------------|
| 默认 UVC + WiFi + BLE + adapter | 通过 | `0x2b5da0` | 32% |
| CDC Device + WiFi + BLE | 通过 | `0x29ef10` | 34% |
| UVC + WiFi-only | 通过 | `0x2a8990` | 34% |
| UVC + BLE-only | 通过 | `0x2aceb0` | 33% |
| UVC + legacy LVGL port | 通过 | `0x2a7650` | 34% |

CDC 构建验证确认 `esp_tinyusb 2.2.0` 必须显式启用 CDC；非 UVC 固件中的 UVC-only UI
helper 已纳入条件编译，复验不再产生未使用函数警告。pytest 启动用例已从只匹配 IDF
入口日志改为等待 UI 创建和 `startup_summary`，但本次未烧录新固件，因此未执行硬件 pytest。

干净配置时，条件 managed component 尚未进入首轮依赖图，TinyUSB、UVC 和 Hosted 的
defaults 可能先报告 `unknown kconfig symbol`，组件解析后会重新运行 Kconfig。必须检查最终
生成的 `sdkconfig`；不能只凭首轮提示判定配置丢失。TinyUSB CDC 两项 defaults 是备用 CDC
构建的必需值，`esp_tinyusb 2.2.0` 不会默认开启 CDC。
