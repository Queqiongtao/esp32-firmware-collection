# 主机端预览工具设计边界

`tools/preview/` 是 P4 800×800 时钟工程的主机端离屏预览，不是固件本体。它复用 `main/` 下的主题源码、设置菜单源码、字体/图片资源和同一版 LVGL 桌面构建，用于快速检查构图和圆屏裁切。

## 与固件的差异

| 项目 | 固件 | 预览 |
| --- | --- | --- |
| 入口 | `main/main.cpp` + `app_main()` | `tools/preview/preview.cpp` |
| 显示后端 | MIPI-DSI 800×800 + LVGL adapter | 800×800 虚拟 display |
| 触摸 | GT9271/GT911 | 无触摸，直接调用菜单 API |
| 时间 | P4 RTC + NTP | 固定 2026-10-20 UTC |
| WiFi | ESP32-C6 + ESP-Hosted | 模拟扫描回调 |
| 音频 | ES8311/PA | 不编译 |
| RTC | P4 内部 RTC | 不编译 |
| 主题过渡 | 800×800 snapshot scale/opacity | 不覆盖 |
| LVGL 配置 | `sdkconfig` | `tools/preview/lv_conf.h` |

预览输出能说明构图、字号、对齐和裁切；不能说明真实亮度、Gamma、刷新率、触摸手感、WiFi 射频和音频。

## 菜单状态

`preview.cpp` 渲染 21 个菜单状态：

- Display：亮度页、Watch faces 两页、候选选中态
- Power：息屏页
- WiFi：关闭页、已连接页、配网摘要、扫描中/结果/空结果/失败、SSID/密码编辑
- Sound：Sound 页、Sound 页底部、Alarm 列表/编辑、Audio test、Timer、Sleep noise

Power 页不再模拟电池；预览不包含 Battery detail 或 Motion 页。

## 维护约定

- 新增主题：同步 `main/main.cpp` 的 `s_themes`、`preview.cpp` 的 `kThemes` 和 `generate_thumbnails.py` 的 `FACES`。
- 新增顶层字体：同步 `main/CMakeLists.txt` 与 `tools/preview/CMakeLists.txt`。
- 修改主题/菜单后重新执行 `./tools/preview/run.sh`；主题视觉变化还应执行 `python3 tools/preview/generate_thumbnails.py` 更新 `clock_theme_previews.c`。
- `report.txt` 的“超出安全区”是构图提示，不是渲染失败；圆屏边缘装饰本来可能接近 400 px 半径。

## 主机测试

```bash
ctest --test-dir tools/preview/build --output-on-failure
```

- `alert_scheduler`：日期去重、并发排队、Snooze、校时、待补发倒计时。
- `menu_and_themes`：表盘选择、异步扫描、Sound 页、闹钟逐分钟编辑、主题反复创建/销毁、点阵 canvas 更新。

测试使用模拟扫描和主机内存，不验证设备射频、音频或真机动画帧率。
