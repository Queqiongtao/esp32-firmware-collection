# 显示方案

## 概述

本工程显示链路基于 MIPI DSI 总线驱动 JD9165 面板控制器，输出 1024×600 RGB565 图像。LVGL 9.4.0 作为 UI 框架，采用 **DIRECT 渲染模式 + 双 framebuffer ping-pong + flush 回调中整 fb cache write back** 的方案，在无撕裂、无残影的前提下保持可接受的帧率。

## 显示链路参数

集中定义于 `main/display_config.h`：

| 参数 | 值 | 说明 |
|------|-----|------|
| 面板型号 | JD9165 | 本地驱动 `components/esp_lcd_jd9165/` |
| 分辨率 | 1024×600 | |
| 颜色格式 | RGB565 | 16bpp，framebuffer 行 stride = 2048 字节 |
| DSI lane 数 | 2 | |
| lane bit rate | 750 Mbps | DPHY 支持 80~1500 Mbps |
| DPI clock | 54 MHz (expect) | 源时钟 PLL_F240M，div=4，real=60MHz，经 bridge HFP 补偿对齐到 60.49Hz |
| framebuffer 数 | 2 | DPI 驱动内部分配于 PSRAM，ping-pong |
| 刷新率 | ≈60.49 Hz | 54e6 / (1384 × 645) |
| H/V 时序 | HSW=40 HBP=160 HFP=160 / VSW=10 VBP=23 VFP=12 | H_total=1384, V_total=645 |

刷新率注释中 "54MHz" 描述的是 expect 值；real DPI 时钟由 240MHz 整分频为 60MHz，IDF 通过 bridge HFP 补偿把每行多塞 154 个 DPI 周期来对齐刷新率到 expect 值。

## 渲染架构

### 当前方案：DIRECT + 双 fb + cache msync

```
LVGL 渲染（只画 dirty 区）
      ↓
flush_cb: esp_cache_msync(整 fb, DIR_C2M)  ← 把 CPU 渲染数据 + sync_areas 的 memcpy 数据写回 PSRAM
      ↓
esp_lcd_panel_draw_bitmap(px_map=fb)      ← IDF 检测到 px_map 在 fb 地址范围，走 do_copy=false
      ↓                                          只设 cur_fb_index，不拷贝
DMA 在整帧传输完成 ISR 切 fb              ← fb 切换发生在帧边界，无撕裂
```

关键点：

- **双 fb 来自 DPI 驱动**：`esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1)` 取 IDF 已分配的 framebuffer，作为 LVGL draw buffer。不再单独分配 partial draw buffer，省下约 320KB PSRAM。
- **flush_cb 只切 fb 地址**：IDF 的 `draw_bitmap` 检测到 `px_map` 在 fb 地址范围内，走 `do_copy=false` 分支，只做 cache write back 并设 `cur_fb_index`，不触发 DMA2D 二次拷贝。
- **fb 切换在帧边界**：DMA 在整帧传输完成 ISR（`mipi_dsi_dma_trans_done_cb`）重启时才读取新 `cur_fb_index`，写入和显示扫描作用于不同 fb，无撕裂。
- **整 fb cache msync**：在 `esp_lcd_panel_draw_bitmap` 之前对整个 fb 做 `esp_cache_msync(DIR_C2M)`，确保 DMA 读到最新数据。这是消除残影的关键（见下文）。

### 方案选型过程

本工程渲染模式经过三次迭代，每一步都有明确的问题驱动：

#### 阶段 0：PARTIAL（原 mipi_dsi 工程继承）

- **配置**：独立分配两个 80 行 partial draw buffer（PSRAM），`LV_DISPLAY_RENDER_MODE_PARTIAL`，DMA2D hook 启用
- **问题 1 - 撕裂**：draw buffer 与 framebuffer 地址无关，IDF 的 `draw_bitmap` 走 `do_copy=true` 路径，用 DMA2D 把每个 80 行小块拷贝到 `fbs[cur_fb_index]`（即 `fbs[0]`，DMA 正在扫描显示的那个 fb）。拷贝与显示扫描并发 → 撕裂。`num_fbs=2` 的第二个 fb 完全闲置。
- **问题 2 - 滑动掉帧 11fps**：整屏重绘需 8 次 render+flush 串行（600/80），叠加 arc 动画 + `lv_scale_set_post_draw` 让 600×600 scale 每帧完整重绘，CPU 渲染（PSRAM）+ DMA2D 拷贝（PSRAM）+ DMA 显示读取（PSRAM）三者竞争 PSRAM 带宽。

#### 阶段 1：DIRECT + 双 fb（消除撕裂，引入残影）

- **配置**：`esp_lcd_dpi_panel_get_frame_buffer` 取双 fb，`LV_DISPLAY_RENDER_MODE_DIRECT`
- **改善**：撕裂消除（fb 切换在帧边界）
- **新问题 - 残影**：三个 arc 动画跑动时出现拖尾。LVGL 内置 `refr_sync_areas` 用 `lv_draw_buf_copy`（CPU memcpy）把上一帧 dirty 区从 on-screen fb 拷到 off-screen fb 保持同步，但 **LVGL 9.4.0 的 `lv_draw_buf_copy` 不做 cache write back**（源码 0 处 `esp_cache_msync` 调用，cache 回调默认为 NULL）。CPU memcpy 写的数据停在 L1 cache，DMA 从 PSRAM 读到旧数据 → 残影。

#### 阶段 2：FULL（消除残影，帧率很低）

- **配置**：`LV_DISPLAY_RENDER_MODE_FULL`
- **改善**：残影消除（每帧整屏重画，两个 fb 内容天然一致，不依赖 sync_areas）
- **新问题 - 帧率很低**：每帧 CPU 整屏渲染 1024×600×2 = 1.2MB，开销过大

#### 阶段 3：DIRECT + 双 fb + flush_cb 整 fb cache msync（最终方案）

- **配置**：回到 DIRECT 模式，在 flush_cb 中 `esp_lcd_panel_draw_bitmap` 之前对整个 fb 做 `esp_cache_msync(DIR_C2M)`
- **效果**：撕裂消除（DIRECT + 双 fb）+ 残影消除（cache write back）+ 帧率回升（只渲染 dirty 区）
- **上板验证通过**：撕裂与残影消除，滑动帧率回升

## 关键问题根因详解

### 撕裂根因（PARTIAL 模式）

PARTIAL 模式下 draw buffer 是独立分配的 PSRAM（与 framebuffer 地址无关）。DPI panel 的 `draw_bitmap`（`esp_lcd_panel_dpi.c`）检测到 draw buffer 不在任何 fb 地址范围内，走 `do_copy=true` 分支，用 DMA2D 拷贝到 `fbs[cur_fb_index]`（一直为 `fbs[0]`）。拷贝与 DMA 显示扫描并发操作同一 fb → 撕裂。`num_fbs=2` 的第二个 fb 在 PARTIAL 模式下完全闲置。

### 残影根因（DIRECT 模式 cache 一致性）

LVGL 9.4.0 在带 cache 的 ESP32-P4 上 cache 管理不完整：

- `lv_draw_buf_copy`（`src/draw/lv_draw_buf.c:571-627`）用 CPU `lv_memcpy` 逐行拷贝，但**不调用任何 cache write back / flush / invalidate**。
- 默认 handlers 的 `flush_cache_cb` 和 `invalidate_cache_cb` 都设为 **NULL**（`src/draw/lv_draw_buf.c:62-65`），即便调用 `lv_draw_buf_flush_cache()` 也是 no-op。
- 整个 LVGL 源码 **0 处** `esp_cache_msync` 调用，LVGL 对 ESP32-P4 cache 一致性完全无感。

DIRECT 模式下 `refr_sync_areas`（`src/core/lv_refr.c:647`）每帧渲染前用 `lv_draw_buf_copy` 把上一帧 dirty 区从 on-screen fb 拷到 off-screen fb。CPU memcpy 写入的数据停在 L1 cache（64 字节行），DMA 从 PSRAM 读取到的是旧数据 → 残影。

**修复**：在 flush_cb 中 `esp_lcd_panel_draw_bitmap` 之前对整个 fb 做 `esp_cache_msync(DIR_C2M)`，把所有 CPU 写入的数据（包括 sync_areas 的 memcpy 和 LVGL 渲染）全部写回 PSRAM。这样 DMA 切 fb 后读到的是最新内容。

### 掉帧根因（PARTIAL 模式）

PARTIAL 模式整屏重绘需 600/80 = 8 次 render+flush 串行。滑动触发 invalidate 时叠加 arc 动画和 `lv_scale_set_post_draw(scale, true)`（`lvgl_demo_ui.c:24`）让 600×600 scale 每帧完整重绘。CPU 渲染（PSRAM）+ DMA2D 二次拷贝（PSRAM）+ DMA 显示读取（PSRAM）三者竞争 PSRAM 带宽。

改用 DIRECT 模式后省掉 DMA2D 二次拷贝，帧率改善。

## 模块结构

显示链路代码分为四个模块，单一职责，不互相直穿内部状态：

| 模块 | 文件 | 职责 |
|------|------|------|
| 板级 | `display_board.c/.h` | MIPI DSI PHY 供电（LDO ch3 @2.5V）、背光 PWM/GPIO、刷新监视 GPIO |
| 面板 | `display_panel.c/.h` | MIPI DSI 总线、DPI 接口、JD9165 面板创建、DMA2D hook |
| LVGL 适配 | `display_lvgl.c/.h` | LVGL 任务、双 fb buffer 分配（DIRECT 模式）、flush 回调（含 cache msync）、tick timer、indev 注册 |
| 触摸 | `display_touch.c/.h` | I2C master 总线、GT911 触摸初始化、坐标缩放、LVGL indev 注册 |

公共参数集中在 `display_config.h`，源文件不出现裸数字或裸 GPIO 号。

## 后续优化方向

以下方向均属独立工作项，应单独提交验证：

### PPA 硬件加速

LVGL 9.4.0 内置 ESP32-P4 专用 PPA（Pixel Processing Accelerator）draw unit（`src/draw/espressif/ppa/`），当前未启用（`CONFIG_LV_USE_PPA` not set）。

- 用途：加速矩形纯色 fill 和 image blit（无圆角/渐变/旋转场景）
- 启用前提：draw buffer 对齐到 64 字节（cache line）。需 `CONFIG_LV_DRAW_BUF_ALIGN=64`、`CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE=64`
- 当前 fb 行 stride 2048 字节已是 64 的整数倍，对齐开销可忽略
- PPA 为实验级、同步阻塞执行，与 SW draw unit 并存，只接管满足条件的任务，不会破坏现有渲染
- 官方文档称矩形 fill 可省约 30%，纯 fill 整数倍屏尺寸场景可达约 9x

### 链路参数调优

当前 DPI 时钟源默认 PLL_F240M=240MHz，整分频 240/4=60MHz，与 expect 54MHz 差 11%，依赖 bridge HFP 补偿。可改用 APLL 108MHz（div=2，real=expect=54MHz）消除 HFP 补偿，使 HFP 回到面板 datasheet 标称值。代价是 APLL 资源被 DPI 独占。

lane rate 理论下限约 432 Mbps（2 lane × 432 = 864 = 54×16），当前 750 Mbps 留约 1.7× 裕度。若需降功耗/EMI 可试 500~600 Mbps，需逐档上板验证。

此类参数调整属高风险试验，按 AGENTS.md 要求须保留 git 回退路径并逐档验证。