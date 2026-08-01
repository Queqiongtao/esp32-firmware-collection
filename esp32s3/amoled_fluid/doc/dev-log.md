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

## 2026-08-01（流体 v1 落地）

- 实现 `components/fluid_render/` 第一版：92x112 Metaball 液态模拟（定点势能场 + 距离查找表）、两步定点双线性放大（92x112 -> 368x448）、256 级调色板（深蓝->青->近白，字节序已交换）、PSRAM 双缓冲全帧提交、模拟(core0)/渲染(core1)双任务
- 触摸接入：main 轮询循环把 down/move/up 注入渲染器；按下绑定最近液滴拖拽、抬起后保持高亮 10 帧再线性衰减，并施加远离手指的推力；无触摸时液滴自动漂移 + 半径呼吸
- 调试能力：`CONFIG_FLUID_RENDER_DEBUG_COLOR_SEQ` 启动时依次显示红/绿/蓝纯色（字节序复验）；`CONFIG_FLUID_RENDER_STATS_INTERVAL_MS` 周期打印 fps/sim/upscale/submit 耗时
- 待办：
  - 真机复验字节序与颜色表现（打开调试模式，正常后关闭）
  - 真机测量帧率、CPU/PSRAM 占用、触摸响应延迟并回写本文

## 2026-08-01（流体 v2 真实感优化）

- 形变：每液滴基础尺寸随机（0.65~1.35 倍），沿运动方向椭圆拉伸（速度相关，上限 `CONFIG_FLUID_RENDER_MAX_STRETCH` 默认 135%），拖动/抬手推力时明显拉长，慢速时恢复圆形
- 光照：每液滴左上角离轴高光（独立 spec 场，`field += spec/2`），高光随液滴运动，不再是同心圆渐变
- 材质：液滴交汇处按主/次贡献占比变暗（`FLUID_CREASE_DARK_Q` 默认 77/256），融合褶皱更像粘稠液体；调色板改为软边缘淡入（36..64），去掉硬轮廓
- 动力学：液滴间弱吸引 + 近距排斥（聚拢成团不重叠）、速度阻尼（*255/256）+ 随机游走 + 最低速度，运动不再匀速直来直去
- 性能：融合变暗的每格除法改为 65536 项倒数查找表（免除法）；仍为全定点热路径
- 构建验证：`./dev.sh build` 通过，`amoled_fluid.bin` 0x47ae0，无编译告警
- 待办：真机对比 v1/v2 视觉与帧率，按需调参（`FLUID_RENDER_MAX_STRETCH`、高光/变暗/凝聚力强度均为代码常量或 Kconfig）

## 2026-08-01（v2.1 修复真机反馈）

真机反馈：液滴核心出现一闪一闪的方形白色块；静放时全部液滴跑到角落。

- 白色方块根因：势能场逐格累加饱和到 255，液滴核心一半区域是平的 255，调色板 160 级以上即近白，核心被冲成白色方块；呼吸/重叠让白块时大时小形成闪烁。修复：
  - 势能场上限压到 `FLUID_FIELD_MAX=192`，液滴本体保持彩色（深蓝->青->亮青），纯白只保留给高光点
  - 高光叠加从 `spec/2` 降为 `spec/4`，高光半径占比 20%->16%
  - 调色板重构：软边缘起点 44，亮青为顶级主体色，近白段只有高光叠加才能达到
- 角落根因：凝聚力让液滴聚团，墙角是双墙稳定平衡点，团一旦贴角即被卡住，其余液滴再被吸引过来。修复：
  - 新增墙排斥：液滴中心距墙小于自身半径时向内推（`FLUID_WALL_K=48`），团无法贴在墙/角
  - 凝聚力作用范围 2.2->1.7 倍半径和，强度 10->6，液滴平时分散、偶尔相遇成团
- 防撕裂加固：面板 IO `trans_queue_depth=10` 为异步 DMA，双缓冲改为**三缓冲**轮转，消除“写缓冲与在传缓冲重叠”的竞争
- 构建验证：`./dev.sh build` 通过，无编译告警
- 待办：真机复验白色方块/角落问题是否消除，微调调色板与力参数

## 2026-08-01（v3 根因修复：方形色块）

真机仍见方形色块，且静放时明显。主机逐像素仿真 + 单液滴最小复现定位到真正的元凶：

- **根因（固件 bug）**：`update_motion_params` 在液滴速度 < 8（8.8 定点）时把单位方向设为 `ux=uy=0`。`compute_field` 用 `along = dx*ux+dy*uy` 计算拉伸距离，方向为 0 时 `along=perp=0` → 每颗液滴 bbox 内**所有格子 d2=0** → 整块 57x57 网格被 255 填满 → 巨大方形色块。液滴一动（ux 恢复单位向量）方块消失，一慢下来又出现——即“一闪一闪的方块”；静放时阻尼把速度拖到阈值以下，方块持续存在
- **修复 1（关键）**：慢速分支改设任意单位方向 `ux=256, uy=0`，配合 stretch=256 退化为标准圆形距离 `d2=dx²+dy²`，不再产生 d2=0
- **修复 2（重叠平台）**：势能累加改为未截断 uint16 `sum`，后期用软饱和 `v=255·s/(s+K)`（K=64，倒数表免除法）替代硬截断，密集重叠区不再出现 255 平台
- 单液滴最小复现验证：修复前 bbox 内 3249 格全部饱和；修复后仅半径内约 113 格有值，呈平滑渐变
- 场景仿真验证：8 液滴静止场景与运动场景，大块同值区占比均归零，液滴呈现深蓝->青->亮青的内部渐变
- 构建验证：`./dev.sh build` 通过，`amoled_fluid.bin` 0x47cf0，无编译告警

## 2026-08-01（v3.1 修复静置聚角落）

真机反馈：方块问题已解决，但静置后所有液滴朝一个角落靠拢并叠成一个点。主机长时仿真（16000 帧）精确复现：质心停在 (5,6) 角落、8 颗液滴最小间距 0.1 格。

- **根因 1（凝聚力单位 bug）**：`apply_cohesion` 中距离 `d` 是 8.8 定点（实际距离 ×256），而 `range/near_d` 是普通格子数，`d > range` 恒成立 → **凝聚力从未生效**，液滴间没有排斥/吸引
- **根因 2（墙角陷阱）**：阻尼 + 撞墙反弹使墙边成为减速滞留区，墙角是双重滞留区；没有向心力把聚团拉回中央，液滴随机漂移后最终全部停在墙角叠成一点
- **修复**：
  - 凝聚力 `range/near_d` 左移 8 位与 `d` 统一单位，排斥/吸引真正生效；参数调整为范围 1.6 倍半径和、近距排斥边界 1.1 倍、吸引 4、排斥 26
  - 新增向心力（`FLUID_CENTER_K=96`，8.8 定点，作用于非触摸液滴）：缓慢把液滴/聚团拉回屏幕中央，打破墙角陷阱
- 仿真验证（正确 isqrt 重跑）：修复后 16000 帧质心距中心约 5 格、散布 13~14 格、最小间距 7~9 格——液滴保持分散停留在屏幕中央区域
- 构建验证：`./dev.sh build` 通过，无编译告警

## 2026-08-01（方案转向：方块物理 + IMU）

用户确认 metaball 流体不是目标效果，新目标为：屏上很多方块，摇动设备（IMU）让方块跟随晃动，模拟物理引擎。

- **方案决策**：直接替换 metaball（删除 `components/fluid_render/`），不做双模式；方块视觉 40~60 个混合大小彩色方块；交互纯 IMU（倾斜=重力方向、晃动=随机冲量），不响应触摸
- **开源选型调研**（详见 `doc/block-physics-design.md`）：Box2D Lite（碰撞/堆叠算法参考）、Android AOSP AccelerometerPlay（加速度计驱动/Verlet 输入处理参考）；不引入完整 Box2D / Chipmunk2D / Physac——ESP32-S3 无 FPU，float 冲量求解 CPU 成本高
- **实现**：新增 `components/block_physics/`——自研定点（Q8.8 位置/速度、Q16.16 质量）AABB 物理：Verlet 积分 + 穿透分离（按质量分摊、每次 80%、3 次迭代）+ 冲量反弹 + 摩擦 + 墙边界；重力来自 IMU 低通滤波（alpha=24/256），晃动来自高通+陀螺能量（阈值 6000、增益 10）；IMU 失联 0.5s 回退默认向下重力；物理 core0 @60Hz、渲染 core1 @30FPS（PSRAM 三缓冲 + 全帧 draw_bitmap）；默认 50 块、尺寸 12~36px、9 色，参数全部进 Kconfig
- **main 改造**：删除触摸轮询，新增 100Hz IMU 采样任务注入 `block_physics_imu_update`；启动流程 board_display -> physics create -> board_imu_init -> imu_task -> physics start
- **文档**：README、doc 索引、display-pipeline、touch-input 同步；`fluid-effect-design.md` 替换为 `block-physics-design.md`
- 构建验证：`./dev.sh build` 通过，无编译告警
- 待办：真机标定 IMU 轴映射（Kconfig `IMU_SWAP_XY/INVERT_X/INVERT_Y`）、调参、测量帧率/CPU/PSRAM

## 2026-08-01（真机标定：IMU 轴映射）

真机反馈：方块沿重力方向的运动与设备实际倾斜方向相差 90°。

- **根因**：QMI8658 的 X/Y 轴与屏幕的 X/Y 方向不对齐（传感器在 PCB 上相对屏幕旋转 90°），低通重力分量直接映射到屏幕坐标导致方向整体偏转 90°
- **修复**：`CONFIG_BLOCK_PHYSICS_IMU_SWAP_XY=y` 写入 `sdkconfig.defaults` 并同步生成后的 `sdkconfig`（注意：既有 sdkconfig 中显式"未设置"会覆盖新默认，需同步修改两处）；重新构建后 `block_physics.cpp` 按交换后的轴编译
- 构建验证：`./dev.sh build` 通过，`amoled_fluid.bin` 0x42670，无编译告警
- 待办：真机复验倾斜/晃动方向；若某轴向仍反向，用 `INVERT_X` / `INVERT_Y` 修正；随后调参与测量帧率/CPU/PSRAM

## 2026-08-01（真机标定：水平方向镜像）

交换 X/Y 后复验：屏幕长边（上下/垂直）方向正确，短边（左右/水平）方向反向——对应传感器相对屏幕为旋转 90° + 水平镜像。

- **修复**：`CONFIG_BLOCK_PHYSICS_IMU_INVERT_X=y` 追加到 `sdkconfig.defaults` 并同步生成后的 `sdkconfig`（同样需同步两处）
- 构建验证：`./dev.sh build` 通过，`amoled_fluid.bin` 0x42670，无编译告警
- 待办：真机复验四个倾斜方向均与滑动方向一致；随后调参与测量帧率/CPU/PSRAM

## 2026-08-01（手感调优：下落提速 + 倾斜跟手）

真机反馈：方块下落感觉很慢、不跟手。

- **慢的原因**：1g 重力加速度仅 18（Q8.8，0.07 px/step²）起步加速度小；速度上限 1024（4 px/step ≈ 240 px/s）压住了终端下落速度，垂直跨屏约需 1.9s
- **不跟手的原因**：IMU 低通 alpha=24（τ≈110ms），重力方向滞后于倾斜动作
- **调优**：
  - `GRAVITY_ACC_Q8` 18 -> 56（起步加速度约 3 倍，满倾终端速度 ~14 px/step）
  - `MAX_VEL_Q8` 1024 -> 2048 并改为 Kconfig（8 px/step ≈ 480 px/s，垂直跨屏约 0.9s）
  - `ACCEL_LP_ALPHA` 24 -> 64（τ≈40ms，倾斜响应明显跟手）
- 构建验证：`./dev.sh build` 通过，`amoled_fluid.bin` 0x42670，无编译告警
- 待办：真机复验手感，按喜好继续调 `GRAVITY_ACC_Q8` / `MAX_VEL_Q8` / `ACCEL_LP_ALPHA` / `DAMPING_Q8`；随后测量帧率/CPU/PSRAM
