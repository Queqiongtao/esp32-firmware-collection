# 显示链路与性能预算

最后更新：2026-08-01

本文是效果渲染开发的核心参考：面板参数、接口带宽、帧缓冲策略与已知字节序坑。

## 面板参数（SH8601）

| 参数 | 值 | 说明 |
| --- | --- | --- |
| 分辨率 | 368 x 448（竖屏） | 列 0..367，页 0..447 |
| 接口 | QSPI（quad SPI） | SCLK=11，D0-D3=4/5/6/7，CS=12 |
| 像素格式 | RGB565（16 bpp） | 帧大小 = 368 x 448 x 2 = 329,728 B ≈ 322 KiB |
| PCLK | 40 MHz | quad 模式下理论 160 Mbit/s = 20 MB/s |
| SPI host | SPI2_HOST | DMA 自动分配，最大传输支持整帧 |
| TE | GPIO13 | 已启用 Tear On（0x35），当前未用于同步 |
| 背光 | SH8601 0x51 命令 | 0..255；无独立背光 PWM GPIO |

## 帧率预算

整帧 322 KiB 经 20 MB/s QSPI 的理论最短耗时：

```text
329,728 B / 20,000,000 B/s ≈ 16.5 ms/帧  ->  理论 ≈ 60 FPS
```

实际帧率还要扣除：命令字节与片选开销、PSRAM 读取速度、CPU 填充帧缓冲耗时、DMA 间隙。全帧刷新目标可先按 **30~50 FPS** 预估，最终以实测为准（记录到 [dev-log.md](dev-log.md)）。

两种降低传输量的手段：

1. **局部刷新**：`0x2A/0x2B` 窗口命令 + `esp_lcd_panel_draw_bitmap(x1,y1,x2,y2,...)` 只刷脏区；方块物理按矩形区域绘制，天然适合局部刷新。
2. **降采样渲染**：先在低分辨率网格/场景上计算，再放大/平滑后提交，CPU 成本大幅下降。

## 帧缓冲策略

- 当前自检代码在 PSRAM 分配整帧缓冲（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`），一次性 `draw_bitmap` 提交。
- 方块物理（v1 已实现）：物理任务（core0，60 Hz 定点求解）发布快照，渲染任务（core1，30 FPS）清黑底后按矩形填充方块，写入 PSRAM 三帧缓冲后全帧 `draw_bitmap` 提交；三缓冲避免“边画边传”的撕裂。
- 若要防撕裂到像素级，可引入 TE 中断/轮询同步（GPIO13），把提交时机对齐到面板帧边界；这是后续讨论项，不在当前骨架实现。
- PSRAM 带宽远高于 QSPI 屏接口，瓶颈通常不在 PSRAM，而在 CPU 填充速度与面板接口；实测优先级：面板接口 > CPU 填充 > PSRAM。

## RGB565 字节序坑（重要）

同款面板的既有工程结论见 `companion_display/firmware/esp32s3/docs/display-color-format.md`。已对照本工程实际拉取的 `waveshare/esp_lcd_sh8601`（v2.0.0）源码核实：

- `panel_sh8601_draw_bitmap` 发送 CASET/RASET 窗口命令后，把调用方缓冲区**原样透传**给 `esp_lcd_panel_io_tx_color`，不做任何字节交换；
- 创建面板时传入的 `data_endian` 字段在该驱动中**未被使用**（未存储、未参与传输）；
- 局部刷新窗口命令（0x2A/0x2B）由 `draw_bitmap` 的 x/y 范围自动生成，支持任意矩形区域。

- SH8601 使用 **RGB565、RGB 元素序**，线路字节为“高字节在前”（big-endian）。
- ESP32-S3 是小端架构，直接把逻辑 `0xF800`（红）交给 `esp_lcd_panel_draw_bitmap` 时，低字节先发，颜色会偏蓝。
- Companion Display 的做法是编译期宏 `SH8601_RGB565()` 交换字节，JPEG 解码路径用 `swap_color_bytes=1`。
- 本工程方块物理渲染层（v1）沿用同一结论：组件内封装 `BLOCK_RGB565()`/`BLOCK_RGB565_WIRE()`，调色板初始化时即完成字节交换，运行时零开销。
- **真机颜色表现仍待复验**：确认方块颜色正常显示后回写本文。

## 当前实现文件

- `components/board_amoled_1_8/board_display.cpp`：SPI 总线、panel IO、SH8601 创建与初始化命令、亮度命令
- `components/block_physics/block_physics.cpp`：AABB 物理求解、方块绘制、双核任务与帧缓冲管理
- `main/main.cpp`：板级初始化、物理编排、IMU 采样注入
