# 外设测试模块总览

## 定位

本工程以显示和触摸为基础，把板载外设封装为相互独立的 `test_<peripheral>` 模块。模块负责硬件生命周期、状态和测试动作，LVGL 只读取状态或触发 capability 接口，不直接访问驱动句柄。

公共硬件参数集中在 `main/test_config.h`，显示与触摸参数集中在 `main/display_config.h`，启用项集中在 `main/Kconfig.projbuild`。详细引脚见 [hardware_pinout.md](./hardware_pinout.md)，USB 专项设计见 [usb_solution.md](./usb_solution.md)。

## 当前能力矩阵

| 能力 | 模块 | Kconfig 默认 | UI/观测入口 | 实现与验证边界 |
|------|------|--------------|-------------|----------------|
| SDMMC SD 卡 | `test_sdcard` | `y` | 启动日志、文件读写校验 | 已实现；挂载/读写和 Hosted Slot 1 共存有上板记录 |
| RX8025T RTC | `test_rtc` | `y` | RTC 卡片、周期日志 | 已实现；状态可查询 |
| ES8311 音频 | `test_audio` | `y` | 音频卡片 | 已实现播放内置 PCM、录音和录音回放 |
| 电池 ADC | `test_battery` | `y` | BAT 卡片 | 已实现；百分比只是线性演示值 |
| USB CDC Device | `test_usb` | USB choice 默认项 | USB Device 卡片 | 单路径有上板记录；与 UVC 共享 HIGH USB |
| USB UVC Host | `test_uvc` | USB choice 可选项 | UVC 独立页面 | 收流/JPEG 有既有记录；最新预览、切换、恢复待上板验证 |
| USB 角色管理 | `test_usb_role` | 任一 USB choice 项启用 | USB 卡片和 UVC 页面 | 已实现异步切换；启动默认 None |
| Hosted WiFi | `test_wireless` | `y`，WiFi 子项 `y` | WiFi 卡片和连接对话框 | 扫描/连接有既有上板记录 |
| Hosted BLE | `test_wireless` | `y`，BLE 子项 `y` | BLE 卡片 | 广播、连接、GATT 测试已实现；最新路径未声明上板通过 |
| MIPI CSI 摄像头 | 未实现 | 无 | 无 | 只有硬件接口能力，尚无 `test_camera` |
| 通用传感器 | 未实现 | 无 | 无 | 尚无 `test_sensor` |

USB `choice EXAMPLE_USB_ROLE` 保证两个配置符号不同时为 `y`，但当前组件仍编译 CDC/UVC 两套源码，角色 worker 启动后保持 None，UI 可以串行请求两种角色。该细节不能解释成“默认启动 CDC”。

## 公共初始化与失败隔离

`app_main()` 和 `app_system_init_task()` 当前顺序如下：

1. 初始化 MIPI DSI 板级电源、面板、背光和 LVGL；LVGL 初始化过程中完成 GT911 与共享 I2C master bus 创建。
2. 创建综合 Demo UI，随后在低优先级后台任务中初始化字体 SPIFFS/FreeType 诊断。
3. 系统初始化任务先准备 NVS，再依次初始化 SD、RTC、Audio、Battery、USB role、Wireless。
4. 每个模块仅在对应 Kconfig 开启时执行。单个模块失败只记录错误并跳过其 `run()`，不会回滚显示或阻止后续模块。
5. Wireless 依赖 NVS；NVS 初始化失败时明确跳过 Wireless。RTC 与 Audio 依赖触摸模块已经创建的共享 I2C bus。

系统任务当前只负责启动，不提供统一的全局 deinit。各模块的 `deinit()` 用于角色切换、局部恢复或未来的受控停机路径。

## 模块说明

### SD 卡

- `test_sdcard_init()` 复用已初始化的 SDMMC host controller，配置 Slot 0、LDO channel 4，并把 FAT 文件系统挂载到 `/sdcard`。
- `test_sdcard_run()` 写入测试文件、读回并校验内容。
- `test_sdcard_deinit()` 卸载文件系统并释放卡、电源控制等资源。
- SD 使用 Slot 0，ESP32-C6 Hosted 使用 Slot 1；不能为 SD 路径重复初始化共享 SDMMC host controller。
- 当前模块没有 `get_status()`，也没有独立 LVGL 卡片，结果以返回值和启动日志为准。

### RTC

- RX8025T-UC 使用共享 I2C bus，7-bit 地址 `0x32`。
- `test_rtc_init()` 创建 device handle 并探测；`test_rtc_run()` 读取时间与状态，时间非法时用固件构建时间初始化 RTC，并启动周期更新。
- `test_rtc_get_status()` 返回初始化、时间有效性、是否从构建时间同步、年月日时分秒和最后错误。
- RTC 卡片轮询状态，不直接发 I2C transaction。

### 音频

- ES8311 控制口复用显示触摸 I2C，数据口使用 I2S0；GPIO20 控制扬声器功放。
- `test_audio_init()` 创建 TX/RX channel、codec 和录音缓冲；`test_audio_run()` 启动后台 worker。
- UI 通过 `test_audio_start_music()`、`test_audio_start_record()`、`test_audio_start_playback()` 投递动作。
- 状态包含当前阶段、录音字节数、峰值、非零样本数和最后错误，可区分“没有输入数据”和播放链路问题。
- 默认 16 kHz、16-bit、stereo，录音时长 5 s；输出音量和 MIC gain 由 Kconfig 控制。

### 电池

- GPIO52 对应 ADC2 channel 3，采样 `BAT -> 68K -> GPIO52 -> 100K -> GND` 的分压中点。
- 每次更新平均 16 个样本，优先使用 ADC calibration；无 calibration 时使用 fallback reference 做粗略换算。
- `test_battery_get_status()` 返回 raw average、ADC mV、换算后的电池 mV、百分比、采样次数和错误。
- 百分比只把 3.3 V 到 4.2 V 线性映射为 0% 到 100%，不是库仑计或精密 fuel gauge，不能用于充放电保护决策。

### USB CDC、UVC 与角色管理

- CDC 和 UVC 共用 HIGH USB controller[0]，必须由 `test_usb_role` 串行释放和申请。
- role worker 的命令 queue 将耗时的 init/deinit、出流和分辨率切换移出 LVGL 任务。
- CDC 状态包括 attach、DTR/RTS 与收发统计；UVC 状态包括枚举、协商、解码、预览、丢帧和恢复状态。
- UVC 页面退出时异步请求 None，由 role worker 释放 Host、JPEG、PPA、task、queue 和 PSRAM buffer；热复位恢复是例外路径。
- 完整生命周期、恢复等级、VBUS 限制和验收场景见 [usb_solution.md](./usb_solution.md)。

### Wireless

- ESP32-P4 与 ESP32-C6 通过 SDIO 4-bit、25 MHz 连接。Hosted transport 是 WiFi 与 BLE 的共同前置条件。
- `test_wireless_init()` 校验 Hosted 配置，连接协处理器，初始化 WiFi STA，并在配置允许时启动 NimBLE Host；`test_wireless_run()` 当前只是兼容统一 `init/run` 入口的状态校验。
- WiFi capability 支持异步扫描、读取最多 12 条结果、连接指定 SSID/密码和断开；状态包含 transport、STA、RSSI、IP、重试次数及协处理器元信息。
- BLE capability 使用 Hosted VHCI，设备名为 `JC1060P470-BLE`，提供可连接广播和一个 128-bit UUID 的 read/write/notify 单字节测试特征。
- BLE 头文件保留 `test_wireless_request_ble_scan()` 接口，但当前业务重点是 Peripheral 广播/GATT；文档不把该接口等同于已完成 Central 扫描产品能力。
- `test_wireless_deinit()` 按依赖逆序停止 NimBLE 和 WiFi，注销 Hosted 事件并释放本模块对象；`esp_hosted` 实例由组件 constructor/destructor 持有，模块不直接销毁其全局生命周期。

## UI 与数据边界

综合页面共有 8 张主卡片：显示与触摸、RTC、USB Device、音频、BAT、WiFi、BLE、FreeType。UVC 使用独立子页面，SD 当前只通过启动日志观测。

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

外设改动后至少执行 `./dev.sh build`。涉及共享 I2C、SDMMC Host、USB controller、DMA/PSRAM 或任务生命周期时，还应覆盖失败初始化、重复进入/退出、设备拔插和资源释放后的再次初始化。显示参数未变化时，不应把外设 UI 的编译结果扩大为显示链路重新完成上板验证。
