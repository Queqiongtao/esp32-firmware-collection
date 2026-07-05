# 显示方案

## 概述

本工程显示链路基于 MIPI DSI 总线驱动 JD9165 面板控制器，输出 1024×600 RGB565 图像。LVGL 9.4.0 作为 UI 框架，采用 **DIRECT 渲染模式 + 双 framebuffer ping-pong + flush 回调中整 fb cache write back** 的方案，在无撕裂、无残影的前提下保持可接受的帧率。

当前 UI 已不是原始 `mipi_dsi` 示例的动画页，而是一个面向板级联调的 **综合验证 Demo**。应用入口 `app_main()` 先完成 `board -> panel -> LVGL` 初始化，再按 Kconfig 选项初始化 `RTC / Audio / Battery / USB / SD / Wireless` 等测试模块，最后加载 `example_lvgl_demo_ui()` 页面。

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

## 当前综合 Demo UI

当前页面由 `main/lvgl_demo_ui.c` 构建，统一使用自定义中文字体，并以纵向滚动页面承载多个验证卡片。

### 页面结构

- **页面根容器**：`screen` 使用 `LV_LAYOUT_FLEX + LV_FLEX_FLOW_COLUMN`，开启 `LV_DIR_VER` 纵向滚动，适配单屏无法一次容纳全部内容的场景。
- **标题区**：`header` 使用纵向 flex 布局，包含两行文案：
  - `JC1060P470 综合验证 Demo`
  - `显示/触摸 + RTC + USB Device CDC + 音频 + BAT + WiFi + BLE`
- **功能卡片区**：`row` 使用 `ROW_WRAP` 布局，当前包含 7 张卡片：
  - `1. 显示与触摸`
  - `2. RTC`
  - `3. USB Device`
  - `4. 音频`
  - `5. BAT`
  - `6. WiFi`
  - `7. BLE`

### 当前交互与布局状态

- **显示与触摸卡片**：展示触摸状态、点击次数、最近坐标和提示文案，并提供一个独立触摸区用于验证 GT911 输入与 LVGL 刷新联动。
- **RTC 卡片**：周期展示初始化状态、当前时间和同步来源。
- **USB Device 卡片**：展示 CDC-ACM 枚举状态、DTR/RTS、最近收发包和累计统计。
- **音频卡片**：展示音频状态、录音数据量、峰值和输入电平，并提供“播放内置音乐 / 开始录音 / 播放录音”三个操作按钮。
- **电量卡片**：展示 `GPIO52` 分压点 ADC 电压、换算后的 BAT 电压和线性估算百分比，用于验证电池采样链路是否正常。
- **WiFi 卡片**：展示 hosted 链路状态、STA 初始化状态、扫描到的 AP 列表，并支持在页面上触发扫描与输入密码连接。
- **BLE 卡片**：当前仅展示 hosted 状态与“待接入”提示，不开放业务操作，避免把未完成路径误写成可用功能。

最近几轮 UI 结构修复如下：

- 页面已开启纵向滚动，解决“向下滑动不了”的问题。
- 标题区 `header` 已改为纵向 flex 布局，避免标题与副标题重叠。
- 第一张“显示与触摸”卡片高度已从 `230` 调整为 `304`，用于容纳 4 行状态文案、提示文案和 `110` 高触摸区，避免“触摸这里”超出卡片边界。
- 无线区域已拆分为独立 `WiFi` / `BLE` 两张卡片，避免把未接入的 BLE 状态混入 WiFi 操作区。

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
| 板级 | `display_board.c/.h` | MIPI DSI PHY 供电（LDO ch3 @2.5V）、背光 PWM/GPIO |
| 面板 | `display_panel.c/.h` | MIPI DSI 总线、DPI 接口、JD9165 面板创建、DMA2D hook |
| LVGL 适配 | `display_lvgl.c/.h` | LVGL 任务、双 fb buffer 分配（DIRECT 模式）、flush 回调（含 cache msync）、tick timer、indev 注册 |
| 触摸 | `display_touch.c/.h` | I2C master 总线、GT911 触摸初始化、坐标缩放、LVGL indev 注册 |

公共参数集中在 `display_config.h`，源文件不出现裸数字或裸 GPIO 号。

## 中文显示问题处理

## 字体分区与 Tiny TTF 方案

当前工程使用独立字体分区承载运行时子集 `TTF`，优先解决 WiFi 动态中文 SSID 缺字问题，并保留后续扩充字符集和字号的空间。

### 当前方案

- `partitions.csv` 新增 `font` SPIFFS 分区，当前大小 `2M`
- `assets/fonts/NotoSansSC-Regular.ttf` 是正式运行时资源，内容为按当前 UI 与常见无线场景裁剪后的子集 `TTF`
- LVGL 启用 `Tiny TTF + Font Manager + FS_STDIO`
- `main/display_font.c/.h` 负责：
  - 挂载字体分区
  - 检查字体文件存在
  - 初始化 Tiny TTF / Font Manager
  - 创建 `16px` UI 字体
- `main/lvgl_demo_ui.c` 统一通过 `display_font_get_ui_font()` 取字体
- `assets/fonts/build_subset.py` 负责：
  - 扫描 `main/*.c`、`main/*.h` 里的 C 字符串字面量
  - 固定保留全部可打印 ASCII
  - 合并 `assets/fonts/ui_font_extra_chars.txt` 里的额外字符
  - 生成 `ui_font_charset.txt` 并调用 `pyftsubset`

### 当前验证边界

- 当前只规划并实现 `16px` 单字号
- 当前不支持任意字号请求
- 当前若字体分区未烧录或字体文件缺失，会回退到 `lvgl_demo_font_patch_16`
- 若无额外说明，应理解为“需经过编译验证和上板验证”，而不是“已长期稳定”

当前综合 Demo 的中文主路径不再依赖 `LV_FONT_DEFAULT` fallback，而是优先走运行时子集 `TTF`；`main/lvgl_demo_font_patch.c` 保留为加载失败时的静态回退字库。

### 问题背景

- 工程已启用 `CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK=y` 与 `CONFIG_LV_FONT_DEFAULT_SOURCE_HAN_SANS_SC_16_CJK=y`。
- 但当前综合 Demo 页面使用的实际汉字集合超过了内建 `Source Han Sans SC 16 CJK` 的覆盖范围，直接使用默认字体会出现缺字或乱码。
- 早期尝试过“补丁字库 + fallback”方案，但在标题、副标题等混排场景下表现不稳定，出现过只显示部分字的现象。

### 当前方案

- 已启用 `CONFIG_LV_USE_TINY_TTF=y`、`CONFIG_LV_USE_FONT_MANAGER=y` 与 `CONFIG_LV_USE_FS_STDIO=y`
- 子集 `TTF` 覆盖：
  - 当前页面实际使用的中文
  - 全部可打印 ASCII、数字和常用标点
  - `ui_font_extra_chars.txt` 中额外保留的一小批常见无线相关中文
- `main/lvgl_demo_ui.c` 通过 `demo_get_ui_font()` 统一返回运行时字体；若加载失败，再回退到 `lvgl_demo_font_patch_16`

### 已处理的显示问题

- **中文缺字/乱码**：主路径改为运行时子集 `TTF`，避免继续维护完整静态字库。
- **中文只显示上半部分**：历史上已修正静态回退字库的垂直度量 `ofs_y` 计算，避免回退场景下字形整体下压。
- **标题首行异常**：经排查主要由标题区两行 label 重叠引起，现已通过 `header` 纵向 flex 布局修复。

### 新增文案的维护约束

- 当前正式字库不是完整中文字体，而是按 `main/` 里的实际字符串和额外补集裁剪出的子集 `TTF`。
- 因此，后续新增 UI 中文文案时，若出现乱码，优先判断是否为“新增字符未进入子集字符集”这一类缺字问题。
- 若新增的是固定 UI 文案，重新运行 `assets/fonts/build_subset.py` 即可自动纳入。
- 若新增的是动态文本场景常用字，例如 WiFi SSID / 设备名里的高频中文，应补到 `assets/fonts/ui_font_extra_chars.txt` 后再重生成子集。
- `lvgl_demo_font_patch_16` 仅作为失败回退保留，不再作为日常增量维护的主路径。

### 当前验证边界

- 上述字体与 UI 修复已经完成 **编译验证**。
- 本文档不把最近几轮字体/UI 调整表述为“全部上板验证通过”；若无额外说明，应理解为“问题已定位并完成静态修改，需以上板结果做最终确认”。

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
