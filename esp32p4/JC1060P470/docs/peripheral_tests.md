# 外设测试模块总览

## 定位

本工程以显示和触摸为基础，把板载外设封装为相互独立的 `test_<peripheral>` 模块。模块负责硬件生命周期、状态和测试动作，LVGL 只读取状态或触发 capability 接口，不直接访问驱动句柄。

板级共享资源集中在 `main/board_config.h` 和 `main/board_i2c.c`，外设参数集中在 `main/test_config.h`，显示与触摸参数集中在 `main/display_config.h`，启用项集中在 `main/Kconfig.projbuild`。详细引脚见 [hardware_pinout.md](./hardware_pinout.md)，USB 专项设计见 [usb_solution.md](./usb_solution.md)。

GT911 与 RX8025T-UC 的 device handle 使用共享总线的 `400 kHz` 配置；ES8311 仍由 `esp_codec_dev` 组件以 `100 kHz` 控制速率注册自己的 device handle。总线由 `board_i2c` 持有，设备模块只借用 handle。

## 当前能力矩阵

| 能力 | 模块 | Kconfig 默认 | UI/观测入口 | 实现与验证边界 |
|------|------|--------------|-------------|----------------|
| SDMMC SD 卡 | `test_sdcard` | `y` | 启动日志、文件读写校验 | 已实现；挂载/读写和 Hosted Slot 1 共存有上板记录 |
| RX8025T RTC | `test_rtc` | `y` | RTC 卡片、周期日志 | 已实现；状态可查询 |
| ES8311 音频 | `test_audio` | `y` | 音频卡片 | 已实现播放内置 PCM、录音和录音回放 |
| 电池 ADC | `test_battery` | `y` | BAT 卡片 | 已实现；百分比只是线性演示值 |
| USB CDC Device | `test_usb` | USB choice 可选项 | HIGH USB 卡片 | 单路径有上板记录；与 UVC 共享 HIGH USB |
| USB UVC Host | `test_uvc` | USB choice 默认项 | UVC 独立页面 | 默认稳定偏好 1280x720；当前 640x360 零错误发布有日志记录，切换与恢复待验证 |
| USB 角色管理 | `test_usb_role` | 任一 USB choice 项启用 | USB 卡片和 UVC 页面 | 已实现异步切换；启动默认 None |
| Hosted WiFi | `test_wireless` | `y`，WiFi 子项 `y` | WiFi 卡片和连接对话框 | 扫描/连接有既有上板记录 |
| Hosted BLE | `test_wireless` | `y`，BLE 子项 `y` | BLE 卡片 | 广播、连接、GATT 测试已实现；当前未声明上板通过 |
| MIPI CSI 摄像头 | 未实现 | 无 | 无 | 只有硬件接口能力，尚无 `test_camera` |
| 通用传感器 | 未实现 | 无 | 无 | 尚无 `test_sensor` |

USB `choice EXAMPLE_USB_ROLE` 保证两个配置符号不同时为 `y`，CMake 和 managed dependencies 只编译所选能力。默认选择 UVC Host，但角色 worker 启动后仍保持 None；UI 只能启停当前固件中存在的 capability。这里的“默认 UVC”是默认编译能力，不表示开机立即占用 HIGH USB。

## 公共初始化与失败隔离

`app_main()` 和 `app_system_init_task()` 当前顺序如下；`app_peripherals_start()` 是 boot-only 编排入口，每次启动只调用一次，不是可从 UI 重复调用的全局重试器：

1. 初始化 MIPI DSI 板级电源、面板和背光，再由 board 层创建共享 I2C master bus；LVGL 初始化过程中只在该总线上创建设备级 GT911 句柄。
2. 创建综合 Demo UI，随后在低优先级后台任务中初始化字体 SPIFFS/FreeType 诊断。
3. 系统初始化任务先准备 NVS，再依次初始化 SD、RTC、Audio、Battery、USB role、Wireless。
4. 每个模块仅在对应 Kconfig 开启时执行。单个模块失败只记录错误并跳过其 `run()`，不会回滚显示或阻止后续模块；失败结果保留在模块状态快照中，供 UI 诊断。
5. Wireless 依赖 NVS；NVS 初始化失败时通过 Wireless 状态报告依赖错误，不创建 hosted/WiFi/BLE 资源。GT911、RTC 与 Audio 都借用 board 层持有的共享 I2C bus，外设模块不创建或销毁总线。
6. 启动任务结束后不会自动再次调用统一装配入口；需要恢复时由对应 capability 的 `init/run/deinit` 或 USB role worker 负责自己的生命周期。

系统任务当前只负责启动，不提供统一的全局 deinit。各模块的 `deinit()` 用于角色切换、局部恢复或未来的受控停机路径。

各模块的 `get_status()` 返回一次性一致快照；未初始化、初始化失败和依赖失败也要保留 `state/last_error/generation/updated_at_us`，而不是用“无数据”覆盖故障原因。`generation` 表示成功生命周期代次，不是每次状态更新的序号。

## 模块说明

### SD 卡

- `test_sdcard_init()` 复用已初始化的 SDMMC host controller，配置 Slot 0、LDO channel 4，并把 FAT 文件系统挂载到 `/sdcard`。
- `test_sdcard_run()` 写入测试文件、读回并校验内容。
- `test_sdcard_deinit()` 卸载文件系统并释放卡、电源控制等资源。
- SD 使用 Slot 0，ESP32-C6 Hosted 使用 Slot 1；不能为 SD 路径重复初始化共享 SDMMC host controller。
- `test_sdcard_get_status()` 返回挂载状态、读写校验结果、字节数、checksum、运行次数和最后错误；当前没有独立 LVGL 卡片，UI/诊断以快照和启动日志为准。

### RTC

- RX8025T-UC 使用共享 I2C bus，7-bit 地址 `0x32`；RTC device handle 请求 `400 kHz`。
- `test_rtc_init()` 创建 device handle 并探测；`test_rtc_run()` 读取时间与状态，时间非法时用固件构建时间初始化 RTC，并启动周期更新。
- `test_rtc_get_status()` 返回初始化、时间有效性、是否从构建时间同步、年月日时分秒和最后错误。
- RTC 卡片轮询状态，不直接发 I2C transaction。

### 音频

- ES8311 控制口复用 board-owned I2C，数据口使用 I2S0；GPIO20 控制扬声器功放。GT911/RTC device handle 请求 `400 kHz`，ES8311 的 `esp_codec_dev` I2C control component 固定以 `100 kHz` 注册 codec device handle；同一条 board-owned bus 支持按 device 配置速率并串行访问。
- `test_audio_init()` 创建 TX/RX channel 和 codec；`test_audio_run()` 启动后台 worker。较大的录音缓冲在首次录音动作时才从 PSRAM 申请。
- UI 通过 `test_audio_start_music()`、`test_audio_start_record()`、`test_audio_start_playback()` 投递动作。
- 状态包含当前阶段、录音字节数、峰值、非零样本数和最后错误，可区分“没有输入数据”和播放链路问题。
- 默认 16 kHz、16-bit、stereo，录音时长 5 s；输出音量和 MIC gain 由 Kconfig 控制。

### 电池

- GPIO52 对应 ADC2 channel 3，采样 `BAT -> 68K -> GPIO52 -> 100K -> GND` 的分压中点。
- 每次更新平均 16 个样本，优先使用 ADC calibration；无 calibration 时使用 fallback reference 做粗略换算。
- `test_battery_get_status()` 返回 raw average、ADC mV、换算后的电池 mV、百分比、采样次数和错误。
- ADC 采样由 Battery worker 周期执行，LVGL timer 只读取快照，不直接触发 ADC 转换。
- 百分比只把 3.3 V 到 4.2 V 线性映射为 0% 到 100%，不是库仑计或精密 fuel gauge，不能用于充放电保护决策。

### USB CDC、UVC 与角色管理

- CDC 和 UVC 共用 HIGH USB controller[0]，但由 Kconfig/CMake 选择其中一个 capability 编译进固件；运行时 `test_usb_role` 只能在该 capability 与 None 之间串行释放和申请。
- role worker 的命令 queue 将耗时的 init/deinit、出流和分辨率切换移出 LVGL 任务。
- CDC 状态包括 attach、DTR/RTS 与收发统计；UVC 状态包括枚举、协商、解码、预览、丢帧和恢复状态。
- UVC 页面退出时异步请求 None，由 role worker 释放 Host、JPEG、PPA、task、queue 和 PSRAM buffer；热复位恢复是例外路径。
- 完整生命周期、恢复等级和 VBUS 限制见 [usb_solution.md](./usb_solution.md)；实测与验收矩阵见 [validation_status.md](./validation_status.md)。

### Wireless

- ESP32-P4 与 ESP32-C6 通过 SDIO 4-bit、25 MHz 连接。Hosted transport 是 WiFi 与 BLE 的共同前置条件。
- `test_wireless_init()` 校验 Hosted 配置并连接协处理器；仅在 `EXAMPLE_WIRELESS_ENABLE_WIFI` 启用时初始化 WiFi STA，仅在 `EXAMPLE_WIRELESS_ENABLE_BLE` 启用时启动 NimBLE Host。WiFi-only、BLE-only 和 WiFi+BLE 都是合法配置；`test_wireless_run()` 当前只是兼容统一 `init/run` 入口的状态校验。
- WiFi capability 支持异步扫描、读取最多 12 条结果、连接指定 SSID/密码和断开；状态包含 transport、STA、RSSI、IP、重试次数及协处理器元信息。
- Hosted transport down/failure 会失效当前连接代次并清除 connected、RSSI 和 IP 快照。WiFi/IP 事件入口必须先通过 transport gate；连接状态提交还要匹配当前请求与 SSID，IP 状态提交进一步校验当前 request generation，避免 transport 失效后的迟到事件恢复旧快照。
- BLE capability 使用 Hosted VHCI，设备名为 `JC1060P470-BLE`，提供可连接广播和一个 128-bit UUID 的 read/write/notify 单字节测试特征。
- 停止 BLE 广告失败时保留 advertising 状态和资源所有权，供后续重试；`BLE_HS_EALREADY` 按已经停止处理。
- BLE 头文件保留 `test_wireless_request_ble_scan()` 接口，但当前业务重点是 Peripheral 广播/GATT；文档不把该接口等同于已完成 Central 扫描产品能力。
- `test_wireless_deinit()` 按依赖逆序停止 NimBLE 和 WiFi，注销 Hosted 事件并释放本模块对象；`esp_hosted` 实例由组件 constructor/destructor 持有，模块不直接销毁其全局生命周期。
- WiFi-only、BLE-only 和组合配置都应独立编译验证；当前上板状态以 [validation_status.md](./validation_status.md) 为准。

## UI 与数据边界

综合页面共有 8 张主卡片：显示与触摸、RTC、HIGH USB、音频、BAT、WiFi、BLE、FreeType。UVC 使用独立子页面，SD 当前没有独立卡片，但可通过 `test_sdcard_get_status()` 和启动日志观测。

UI 定时器只调用 `get_status()`、读取快照或提交异步请求。以下操作不得直接放在 LVGL timer/event callback 中：

- USB stack install/uninstall、UVC 等待设备和 stream reopen。
- 音频录制或播放循环。
- WiFi 扫描等待和连接重试。
- SD 文件读写与挂载。

触摸由 `display_touch` 缓存硬件数据并完成坐标变换；UI/LVGL 不访问 GT911 handle 私有状态。UVC 预览通过 `test_uvc_copy_preview()` 复制到 UI 自有 buffer，UI 不持有 JPEG DMA 输出缓冲。

## 验证要求

文档中的状态按以下口径使用：

- **已实现**：源码和接口存在，不代表硬件可用或长期稳定。
- **编译验证通过**：`./dev.sh build` 通过，仅证明当前配置可编译链接。
- **已上板验证**：必须有实际设备观测记录，并说明测试路径；不能由旧版本或相邻模块推断。

当前可读取 FULL USB 和摄像头运行日志，但本次审查不覆盖板上固件。修改后的结论只按静态检查和多配置编译记录；显示参数保持 `num_fbs=2`、lane rate `750 Mbps`、DPI clock `54 MHz`，外设结果不得扩大为显示链路的新上板结论。

外设改动后至少执行 `./dev.sh build`。涉及共享 I2C、SDMMC Host、USB controller、DMA/PSRAM 或任务生命周期时，还应覆盖失败初始化、重复进入/退出、设备拔插和资源释放后的再次初始化。显示参数未变化时，不应把外设 UI 的编译结果扩大为显示链路重新完成上板验证。
