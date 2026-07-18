# 显示方案

## 概述

本工程显示链路基于 MIPI DSI 总线驱动 JD9165 面板控制器，输出 1024×600 RGB565 图像。当前使用 **LVGL 9.4.0 + espressif/esp_lvgl_adapter 0.6.2**，默认走 `esp_lvgl_adapter` 的 **DOUBLE_DIRECT + 双 framebuffer** 路径，并启用 ESP LCD DMA2D hook 与 adapter PPA 加速提示。该组合有既有上板记录，记录中未观察到白条、花屏、闪烁、黑屏、撕裂或残影。

当前显示参数保持 `num_fbs=2`、lane rate `750 Mbps`、DPI clock `54 MHz (expect)`。显示路径、外设路径和 UVC 预览的验证证据分别记录，不能由外设编译结果推断显示链路重新完成上板验证；统一状态见 [validation_status.md](./validation_status.md)。

当前 UI 已不是原始 `mipi_dsi` 示例的动画页，而是面向板级联调的 **综合验证 Demo**。`app_main()` 先完成 `board -> panel -> LVGL` 初始化并创建 UI，再启动后台 FreeType 初始化任务和外设初始化任务。SD、RTC、Audio、Battery、USB role、Wireless 的初始化不会阻塞首屏创建；单个外设失败只记录错误，不阻断其他模块。

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

## 当前 LVGL 与加速状态

| 项目 | 当前值 | 说明 |
|------|--------|------|
| LVGL | `lvgl/lvgl 9.4.0` | `main/idf_component.yml` 固定版本 |
| LVGL adapter | `espressif/esp_lvgl_adapter ^0.6.2` | 0.1.4 与 IDF 6.0.1 的 DMA2D/FBCPY API 不兼容，已升级 |
| 默认 LVGL port | `CONFIG_EXAMPLE_LVGL_PORT_ESP_ADAPTER=y` | 自研 legacy port 保留为 Kconfig 回退 |
| tear mode | `DOUBLE_DIRECT` | 当前 `num_fbs=2`，接近原稳定 DIRECT 双 fb 行为 |
| ESP LCD DMA2D hook | `CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME=y` | 调用 `esp_lcd_dpi_panel_enable_dma2d()`，用于 adapter/partial/full 等需要 `draw_bitmap` 拷贝的路径 |
| adapter PPA | `CONFIG_EXAMPLE_LVGL_ADAPTER_ENABLE_PPA_ACCEL=y` | 由 `esp_lvgl_adapter` 接管 PPA 加速 hook |
| LVGL 内置 PPA | `CONFIG_LV_USE_PPA` 未启用 | 不走 LVGL 上游 `src/draw/espressif/ppa/` 后端 |
| SW draw unit | `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1` | 减少并行 draw unit 对 PSRAM/cache 路径的额外压力 |

注意：ESP32-P4 上不要启用 STM32 专用的 `LV_USE_DRAW_DMA2D`。本工程的 DMA2D 来自 ESP LCD DPI panel 的 `draw_bitmap` hook；PPA 加速由 `esp_lvgl_adapter` 负责。若要试验 `TRIPLE_PARTIAL` 或 `DOUBLE_FULL`，应通过 `Example Configuration -> esp_lvgl_adapter tear avoidance mode` 单独切换，并保留当前 `DOUBLE_DIRECT` 回退路径。

## 当前综合 Demo UI

当前页面由 `main/lvgl_demo_ui.c` 构建，统一使用自定义中文字体，并以纵向滚动页面承载多个验证卡片。

### 页面结构

- **页面根容器**：`screen` 使用 `LV_LAYOUT_FLEX + LV_FLEX_FLOW_COLUMN`，开启 `LV_DIR_VER` 纵向滚动，适配单屏无法一次容纳全部内容的场景。
- **标题区**：`header` 使用纵向 flex 布局，包含两行文案：
  - `JC1060P470 综合验证 Demo`
  - `显示/触摸 + RTC + USB/UVC + 音频 + BAT + WiFi + BLE`
- **功能卡片区**：`row` 使用 `ROW_WRAP` 布局，当前包含 8 张卡片：
  - `1. 显示与触摸`
  - `2. RTC`
  - `3. HIGH USB`
  - `4. 音频`
  - `5. BAT`
  - `6. WiFi`
  - `7. BLE`
  - `8. FreeType`
- **UVC 子页面**：从 HIGH USB 卡片进入，包含 RGB565 预览、协商状态、解码/丢帧/恢复计数和摄像头支持的 MJPEG 分辨率列表。

### 当前交互与布局状态

- **显示与触摸卡片**：展示触摸状态、点击次数、最近坐标和提示文案，并提供一个独立触摸区用于验证 GT911 输入与 LVGL 刷新联动。
- **RTC 卡片**：周期展示初始化状态、当前时间和同步来源。
- **HIGH USB 卡片**：展示 controller[0] 当前状态及所选 capability 摘要，可启停当前编译的 CDC 或 UVC 能力。USB role worker 异步执行底层 init/deinit，UI 不直接持有 USB 句柄；另一角色若未编译则不可在运行时切换。
- **音频卡片**：展示音频状态、录音数据量、峰值和输入电平，并提供“播放内置音乐 / 开始录音 / 播放录音”三个操作按钮。
- **电量卡片**：展示 `GPIO52` 分压点 ADC 电压、换算后的 BAT 电压和线性估算百分比，用于验证电池采样链路是否正常。
- **WiFi 卡片**：展示 hosted 链路状态、STA 初始化状态、扫描到的 AP 列表，并支持在页面上触发扫描与输入密码连接。
- **BLE 卡片**：展示 hosted NimBLE、广播/连接和测试特征值状态，可启停广播；该软件路径已实现，是否完成当前版本上板验证以验证状态表为准。
- **FreeType 卡片**：展示字体分区、FreeType 初始化诊断、动态字号样本和双指缩放结果。普通 UI 仍使用静态 16 px 补丁字体，避免后台字体失败影响主界面。

最近几轮 UI 结构修复如下：

- 页面已开启纵向滚动，解决“向下滑动不了”的问题。
- 标题区 `header` 已改为纵向 flex 布局，避免标题与副标题重叠。
- 第一张“显示与触摸”卡片高度已从 `230` 调整为 `304`，用于容纳 4 行状态文案、提示文案和 `110` 高触摸区，避免“触摸这里”超出卡片边界。
- 无线区域已拆分为独立 `WiFi` / `BLE` 两张卡片，分别承载 STA 操作与 hosted NimBLE 广播/GATT 状态。

## 渲染架构

### 当前默认方案：esp_lvgl_adapter + DOUBLE_DIRECT

```
app_main()
      ↓
display_panel_init()
      ├─ 创建 MIPI DSI / DBI IO / JD9165 DPI panel
      ├─ 按 adapter tear mode 计算 num_fbs（当前 DOUBLE_DIRECT → 2）
      └─ esp_lcd_dpi_panel_enable_dma2d()
      ↓
display_lvgl_init()
      ├─ esp_lv_adapter_init()
      ├─ esp_lv_adapter_register_display(MIPI, DOUBLE_DIRECT, PPA enabled)
      ├─ esp_lv_adapter_register_touch(GT911)
      └─ esp_lv_adapter_start()
```

关键点：

- **adapter 是默认路径**：`display_lvgl.c` 默认通过 `esp_lvgl_adapter` 注册 display/touch，统一使用其 display bridge、thread lock、tear avoidance 和 PPA helper。
- **DOUBLE_DIRECT 作为稳定默认**：该模式需要 2 个 DPI framebuffer，保持低拷贝负载，且接近前一版已验证的 DIRECT 双 fb 行为。
- **DMA2D hook 仍启用**：`display_panel.c` 调用 `esp_lcd_dpi_panel_enable_dma2d()`。在 DOUBLE_DIRECT 下不强制制造额外拷贝；切到 `TRIPLE_PARTIAL` 或 `DOUBLE_FULL` 做对比时，DMA2D copy 负载会更明显。
- **PPA 由 adapter 接管**：`disp_cfg.profile.enable_ppa_accel = true`，不启用 LVGL 内置 PPA draw backend。
- **legacy 可回退**：`CONFIG_EXAMPLE_LVGL_PORT_CUSTOM_LEGACY` 保留旧自研 DIRECT 双 fb + flush cache msync 路径，用于排查 adapter 问题。

### 方案选型过程

本工程渲染模式经过多次迭代，每一步都有明确的问题驱动：

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

#### 阶段 3：自研 DIRECT + 双 fb + flush_cb 整 fb cache msync（legacy 回退）

- **配置**：回到 DIRECT 模式，在 flush_cb 中 `esp_lcd_panel_draw_bitmap` 之前对整个 fb 做 `esp_cache_msync(DIR_C2M)`
- **效果**：撕裂消除（DIRECT + 双 fb）+ 残影消除（cache write back）+ 帧率回升（只渲染 dirty 区）
- **上板验证通过**：撕裂与残影消除，滑动帧率回升

#### 阶段 4：esp_lvgl_adapter 0.6.2 + DOUBLE_DIRECT（当前默认）

- **配置**：引入 `espressif/esp_lvgl_adapter ^0.6.2`，默认 `EXAMPLE_LVGL_PORT_ESP_ADAPTER`，tear mode 为 `DOUBLE_DIRECT`，adapter PPA enabled。
- **原因**：乐鑫 adapter 已覆盖 ESP32-P4/MIPI DSI/LVGL 9 的 display bridge、tear avoidance、touch 注册与 PPA helper，比继续维护自研 port 更接近官方路径。
- **版本注意**：`esp_lvgl_adapter 0.1.4` 会在 IDF 6.0.1 下因 `esp_async_fbcpy_trans_desc_t` 字段变更编译失败，因此当前锁定到 `^0.6.2`。
- **既有上板记录**：该组合在记录对应版本中未观察到白条、花屏、闪烁、黑屏、撕裂或残影；后续改动是否重新上板以验证状态文档为准。

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
| LVGL 适配 | `display_lvgl.c/.h` | 默认注册 `esp_lvgl_adapter` display/touch；legacy 路径保留自研 LVGL task、flush、indev |
| 触摸 | `display_touch.c/.h` | 在 board-owned I2C 总线上以 400 kHz device handle 创建 GT911、读取硬件、坐标缩放和只读快照；控制器句柄不对外暴露 |

共享 I2C 的板级参数集中在 `board_config.h`，显示与触摸参数集中在 `display_config.h`，源文件不出现裸数字或裸 GPIO 号。GT911/RTC device handle 使用 400 kHz；ES8311 的 `esp_codec_dev` I2C control component 使用 100 kHz，三者仍由同一 board-owned bus 串行仲裁。

## 中文字体与 FreeType 诊断

### 当前架构

字体路径分为“主 UI 静态字体”和“FreeType 动态诊断”两层：

- 普通 UI 统一使用 `main/lvgl_demo_font_patch.c` 的 `lvgl_demo_font_patch_16`。主界面不依赖 SPIFFS 或 FreeType 初始化结果，字体分区异常时仍可显示。
- `partitions.csv` 提供 `2M` 的 `font` SPIFFS 分区，构建时由 `spiffs_create_partition_image(font ../assets/fonts FLASH_IN_PROJECT)` 打包。
- 运行时资源为 `assets/fonts/ui_shs_sc.otf`，挂载路径 `/spiffs/ui_shs_sc.otf`，Kconfig 文件名为 `CONFIG_EXAMPLE_FONT_FILE_NAME`。
- LVGL 当前启用 `FreeType + FS_STDIO`；`CONFIG_LV_USE_TINY_TTF=n`、`CONFIG_LV_USE_FONT_MANAGER=n`，不再使用早期 Tiny TTF 方案。
- `display_font_init()` 在低优先级后台任务中挂载分区、检查文件、初始化 FreeType，并创建 `16/16/18/22 px` 的 Small、Body、Card Title、Title 诊断字体。
- FreeType 卡片可按 14~240 px、步进 2 px 请求动态样本字体；模块缓存有限数量的字号对象并回收旧对象。双指手势期间只按实时指距对当前字体做 transform 预览，松手后再创建或复用最终字号，避免在触摸高频路径同步创建和淘汰 FreeType 字体。

`display_font_get_role_font()` 返回静态 UI 字体；只有显式调用 `display_font_get_freetype_role_font()` 或 `display_font_get_freetype_sample_font()` 的诊断控件使用运行时 OTF。这个边界避免把字体文件、SPIFFS 和 FreeType 的失败扩散到主 UI。

### 字符集维护

仓库保留的是思源黑体简中 Regular 的子集 OTF，而不是完整官方字体：

- `assets/fonts/build_subset.py` 扫描 `main/*.c`、`main/*.h` 的 C 字符串字面量，固定加入全部可打印 ASCII，并合并 `assets/fonts/ui_font_extra_chars.txt`。
- 合并结果写入 `assets/fonts/ui_font_charset.txt`，随后由 `pyftsubset` 生成 `ui_shs_sc.otf`。
- 固定 UI 文案发生变化时，需要同时用该字符集重新生成静态 `lvgl_demo_font_patch_16`；仅更新 OTF 不会补齐主 UI 静态字库。
- 动态 SSID、设备名等无法从源码扫描出的常用字符，应先加入 `ui_font_extra_chars.txt`。

完整生成命令和许可证要求见 `assets/fonts/README.md`。

### 已处理问题与验证边界

- 静态补丁字库已修正历史上的字形垂直度量问题；标题区已改为纵向 flex，避免两行 label 重叠。
- FreeType 初始化失败时，诊断卡片显示静态 fallback，不影响其他外设卡片和触摸操作。
- 当前显示链路的 DOUBLE_DIRECT 组合有既有上板验证记录。FreeType 动态字号和 UVC 子页面必须按各自记录判断；当前 UVC 640x360 有运行日志，但不等价于 FreeType 手势、720p/1080p 或长期刷新全部通过。

## 后续优化方向

以下方向均属独立工作项，应单独提交验证：

### PPA / DMA2D 后续试验

当前已经启用 **adapter PPA** 与 **ESP LCD DMA2D hook**，但没有启用 LVGL 内置 PPA draw backend。

- `CONFIG_EXAMPLE_LVGL_ADAPTER_ENABLE_PPA_ACCEL=y`：把 PPA 加速提示交给 `esp_lvgl_adapter`，当前默认开启。
- `CONFIG_EXAMPLE_USE_DMA2D_COPY_FRAME=y`：启用 ESP LCD DPI panel 的 DMA2D `draw_bitmap` hook，当前默认开启。
- `CONFIG_LV_USE_PPA` 不启用：LVGL 9.4.0 自带的 `src/draw/espressif/ppa/` 后端在当前组合下需要额外对齐和组件依赖处理，且与 adapter PPA 职责重叠，暂不作为默认路径。
- `CONFIG_EXAMPLE_LVGL_ADAPTER_FORCE_PARTIAL_DMA2D_TEST` 仅用于对比 DMA2D copy 负载，不建议作为日常默认。

### 链路参数调优

当前 DPI 时钟源默认 PLL_F240M=240MHz，整分频 240/4=60MHz，与 expect 54MHz 差 11%，依赖 bridge HFP 补偿。可改用 APLL 108MHz（div=2，real=expect=54MHz）消除 HFP 补偿，使 HFP 回到面板 datasheet 标称值。代价是 APLL 资源被 DPI 独占。

lane rate 理论下限约 432 Mbps（2 lane × 432 = 864 = 54×16），当前 750 Mbps 留约 1.7× 裕度。若需降功耗/EMI 可试 500~600 Mbps，需逐档上板验证。

此类参数调整属高风险试验，按 AGENTS.md 要求须保留 git 回退路径并逐档验证。
