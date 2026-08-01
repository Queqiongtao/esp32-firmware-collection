# 开发日志

最后更新：2026-08-01

规则：每次开发/实验按日期追加条目；测量数据必须包含条件（分辨率、帧率、CPU 占用、功耗、固件版本/commit）。与正文文档相关的结论择机回写。

## 2026-08-01

- 创建 `amoled_fluid` 工程骨架：以 `waveshare_amoled_1_8` 为模板，完整复制 `components/board_amoled_1_8/` 板级组件与 `sdkconfig.defaults`，`main` 保留显示自检 + 触摸轮询基线，日志 TAG 改为 `amoled_fluid`
- 文档：建立 `doc/` 完整文档集（索引、pinout、启动时序、显示链路、触摸、流体方案、依赖构建、开发日志）
- 依赖调整：移除参考工程清单中代码未使用的 `waveshare/esp32_s3_touch_amoled_1_8` BSP，显式声明 `waveshare/esp_lcd_sh8601 ^2`、`espressif/esp_lcd_touch ^1`、`espressif/esp_lcd_touch_ft5x06 ^1`（与 companion_display 已验证清单一致）
- 构建验证：本机无网络，复用 `companion_display/firmware/esp32s3` 的 `managed_components` + `dependencies.lock`，按本工程清单重算 `manifest_hash` 并同步 `direct_dependencies`，以 `IDF_COMPONENT_CHECK_NEW_VERSION=0 ./dev.sh build` 完成构建；`amoled_fluid.bin` 正常生成（大小 0x45360）
- 驱动核对：`waveshare/esp_lcd_sh8601` v2.0.0 的 `draw_bitmap` 原样透传颜色缓冲区、不使用 `data_endian`，与显示链路文档结论一致
- 待办：
  - 真机实测 RGB565 字节序表现，确认是否沿用参考工程的直传写法，结论回写显示链路文档
  - 真机测量全帧刷新的实际帧率与 CPU/PSRAM 占用
