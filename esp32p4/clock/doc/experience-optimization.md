# P4 移植与体验记录

## 2026-11-08：从 S3 移植到 ESP32-P4 800×800

### 已完成

- 新建 `esp32p4/clock` 独立工程，使用 managed BSP `waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1。
- 屏幕配置为 800×800、2-lane MIPI-DSI、80 MHz DPI、RGB565、3 个 DPI buffer。
- Flash 32 MB，PSRAM 250 MHz HEX，factory app 8 MB，partition table offset `0x10000`。
- 项目名改为 `esp-clock-p4`，输出 `build/esp-clock-p4.bin`。
- 移除 S3 的 `qmi8658`、`pcf85063`、`attitude`、`esp_io_expander`/XPowersLib、本地 AMOLED BSP。
- 移除 `imu_demo`、`power_manager`、`spring_angle`、Motion 页、电池卡片和 Battery detail。
- `clock_settings_t` 删除 `auto_rotate`，不再读写 NVS `auto_rotate`。
- 主题与菜单按 466 设计网格编译期换算到 800，删除父容器 `transform_scale`。
- 重新生成 800 布局字体：Minimal 248 px、Segment 165/62 px、M5 185/137/48/41 px、菜单大数字 62 px。
- 设置菜单改为 4 页：Display、Power、WiFi、Sound；底部 4 个 Tab。
- 触摸阈值按 800 放大：滑动 172 px、轻扫 86 px、长按容差 28 px；触摸层 800×800。
- BOOT 键映射 GPIO35；无 PWR/EXIO4/AXP 逻辑。
- WiFi 使用 `esp_wifi_remote` + `esp_hosted`，启动时连接 C6、等待传输就绪并打印 STA MAC。
- RTC 改为 P4 内部 RTC；NTP 同步后系统时间由 RTC 域保持。
- 主机预览改为 800×800，支持 8 个主题和 21 个菜单状态。
- 主机测试 `alert_scheduler`、`menu_and_themes` 通过。

### 验证结果

- `./dev.sh build` 通过（ESP-IDF 6.1）。
- 真机 `/dev/ttyACM0` 烧录成功，芯片识别为 ESP32-P4 v1.0，CPU 360 MHz，PSRAM 32 MB @ 200 MHz。
- 800×800 JD9365 显示、GT911 触摸、ES8311 扬声器初始化成功。
- ESP-Hosted 通过 SDIO slot 1 / 4-bit / 40 MHz 连接 C6，读到 STA MAC。
- 触摸滑动切换主题正常；日志显示 UI 音效播放。
- `esp-clock-p4.bin` 约 2.9 MB，8 MB 分区剩余约 64%。
- `menu_and_themes` 发现并修复了表盘选择卡片、闹钟编辑、Timer 预设、Audio test、WiFi 扫描状态和 Sound 音量标签在 800 下的重叠/裁切问题。
- 仍需验证：C6 协处理器固件版本报告为 0.0.0（Host 2.13.0），需要实际 WiFi 扫描/连接/NTP；P4 RTC 掉电保持需装 RTC 电池后测试。

### 2026-11-08 补充修复

- Dot Matrix 秒数：`render_digits()` 的列参数误用了 `kSmallPitch`，第二个数字从第 10 列开始并被 canvas 裁切；改为设计网格列 `{0, 6}`。
- Dot Matrix 冒号：改为时间 canvas 的子对象，固定列 32、行 8/16，避免与主画布坐标脱节；奇数秒暗态颜色调亮。
- Audio test 重启：将 LVGL task stack 从 8 KB 提升到 16 KB，audio task stack 从 8 KB 提升到 12 KB；自动打开 Audio test 并连续播放多个测试音验证不再复位。

### 上板验收清单

1. 显示方向、圆形裁切、8 个主题构图和字体清晰度。
2. 触摸坐标、滑动/轻扫切换、长按菜单；BOOT 键单击/长按。
3. ES8311 扬声器、PA GPIO53、各音效/闹钟/白噪音。
4. ESP32-C6 Hosted 传输、STA MAC、扫描、连接、断线重连。
5. NTP 同步、P4 RTC 掉电保持（装/不装 RTC 电池）。
6. 菜单 4 页、图标 Tab、Done/超时关闭、WiFi 键盘。
7. 睡眠/唤醒亮度渐变、白噪音在 USB 供电下的行为。
