# SH8601 显示颜色格式

## 结论

本工程的 SH8601 面板使用 **RGB565**，颜色元素顺序为 **RGB**，不是 BGR。实际发送到 QSPI 面板的数据采用每个 RGB565 像素的高字节在前（big-endian）顺序。

ESP32-S3 为小端架构：直接将逻辑 RGB565 `uint16_t` 值传给当前 SH8601 QSPI 驱动时，内存中的低字节会先发送，导致颜色异常。例如逻辑红色 `0xF800` 会被发送为 `00 F8`，面板将其解释为偏蓝的颜色。

## 当前实现

`components/subscreen_display/subscreen_display.c` 定义 `SH8601_RGB565()` 宏。本地 UI 的所有 RGB565 常量都通过该宏在编译期交换两个字节，随后可直接由 DMA 发送：

| 逻辑 RGB565 | 面板发送值（`uint16_t`） | 内存/线路字节 |
| --- | --- | --- |
| 红 `0xF800` | `0x00F8` | `F8 00` |
| 绿 `0x07E0` | `0xE007` | `07 E0` |
| 蓝 `0x001F` | `0x1F00` | `00 1F` |

该转换仅作用于编译期常量，不增加运行时逐像素处理、额外帧缓冲或 CPU 带宽消耗。

JPEG 视频帧由 `esp_jpeg_decode()` 解码为 RGB565，并设置 `.flags.swap_color_bytes = 1`。解码器直接输出相同的面板字节序，因此视频帧不应再经过额外的字节交换。

## 约束

- 新增本地 RGB565 颜色值必须使用 `SH8601_RGB565()`，不得将逻辑 RGB565 常量直接交给 `esp_lcd_panel_draw_bitmap()`。
- JPEG 路径必须保留 `swap_color_bytes = 1`，除非同时调整所有本地绘制与面板传输策略。
- `board_display.cpp` 中的 `data_endian` 不会被当前 `waveshare/esp_lcd_sh8601` 组件用于 QSPI 色彩数据传输；该组件直接提交调用方缓冲区。因此字节序约定由本地颜色宏和 JPEG 解码配置共同保证。
