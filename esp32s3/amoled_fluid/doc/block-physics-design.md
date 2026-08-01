# 方块物理效果方案设计（IMU 驱动）

最后更新：2026-08-01

本文是 `components/block_physics/` 的目标蓝图与实现说明：屏幕上分布 40~60 个混合大小彩色方块，摇动/倾斜设备（QMI8658 IMU）时方块跟随重力与晃动冲量运动，互相碰撞堆叠，模拟物理引擎效果。

交互仅包含 IMU，不响应触摸。

## 开源方案选型调研（结论：参考算法，不引入依赖）

| 方案 | 说明 | 结论 |
| --- | --- | --- |
| Box2D Lite（erincatto/box2d-lite） | 完整 Box2D 作者的教学版，约 1000 行，含 SAT 碰撞、冲量求解、摩擦/堆叠 | **碰撞/堆叠算法参考**，不引入 |
| 完整 Box2D | 最知名的 2D 刚体引擎（MIT），功能全但体积大、依赖 float | 不引入：ESP32-S3 无 FPU |
| Chipmunk2D | 老牌 C 语言 2D 引擎，结构较简洁 | 可参考，不引入 |
| Physac（raylib 生态） | 已有 ESP-IDF 组件打包（yanke01/physac），float 实现 | 可参考，不引入 |
| AOSP AccelerometerPlay | Android 官方示例：低通取重力、高通取晃动，加速度计驱动小球 | **IMU 输入处理参考** |

关键原因：ESP32-S3 没有 FPU，float 运算是软件模拟的；60Hz × 50 个方块的浮点冲量求解会吃掉大量 CPU。因此本工程自研定点（Q8.8/Q16.16）AABB 求解器，算法照抄 Box2D Lite 的穿透分离 + 冲量反弹 + 摩擦思路，输入处理照抄 AccelerometerPlay 的低通/高通思路。后续若需要旋转体、复杂形状或关节，再评估引入完整引擎。

## 物理模型（core0，60 Hz，定点）

- 方块为**轴对齐 AABB，不旋转**；位置/速度用 Q8.8 定点（1/256 px 精度），质量 = 面积，`inv_mass` 用 Q16.16
- 每步积分：重力加速度 + 晃动随机冲量 -> 速度阻尼（默认 252/256）-> 限速 -> 位移
- 碰撞（每步 2~3 次迭代）：重叠对沿穿透最小轴做位置修正（按质量分摊，每次修正 80%），法向速度按恢复系数反弹，切向按摩擦系数衰减
- 四边为墙：位置夹取 + 法向反弹 + 切向摩擦
- IMU 失联（>0.5s 无数据）时回退默认重力（垂直向下），效果仍可运行

## IMU 输入管线（main 采样任务，100 Hz）

- `board_imu_read` 原始 6 轴数据（QMI8658，±2g / ±2000dps，16384 LSB/g）注入 `block_physics_imu_update`
- 轴映射/符号由 Kconfig 标定（`BLOCK_PHYSICS_IMU_SWAP_XY` / `INVERT_X` / `INVERT_Y`），首次真机实测确认
- 低通滤波（alpha=24/256，τ≈110ms）取屏幕平面重力分量，反推方块加速度：`grav = lp * G_Q8 >> 14`
- 高通滤波 + 陀螺仪幅度合成晃动能量；能量超阈值时对每个方块施加随机方向冲量，能量按步衰减（250/256）

## 任务架构与渲染（core1，30 FPS）

- 物理任务（`block_phys`，core0，优先级 5）与渲染任务（`block_render`，core1，优先级 6）通过互斥锁 + 快照缓冲交接方块状态（物理每步发布，渲染按需拷贝）
- 渲染：PSRAM 三帧缓冲 + 全帧 `draw_bitmap`；每帧清黑底后按方块矩形填充（调色板初始化时完成 RGB565 字节交换，运行时零开销）
- 启动流程：`board_display_init` -> `board_display_new` -> `block_physics_create` -> `board_imu_init` -> IMU 采样任务 -> `block_physics_start`

## 参数表（Kconfig，`BLOCK_PHYSICS_*`）

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| BLOCK_COUNT | 50 | 方块数量（40~60） |
| BLOCK_MIN_SIZE / MAX_SIZE | 12 / 36 | 方块尺寸范围（px） |
| COLOR_COUNT | 9 | 调色板颜色数 |
| PHYSICS_HZ | 60 | 物理更新频率 |
| RENDER_HZ | 30 | 渲染目标帧率 |
| ITERATIONS | 3 | 每步碰撞迭代次数 |
| GRAVITY_ACC_Q8 | 18 | 1g 重力加速度（Q8.8 px/step²） |
| DAMPING_Q8 | 252 | 速度阻尼（Q8.8） |
| RESTITUTION_Q8 | 32 | 反弹恢复系数（Q8.8） |
| FRICTION_Q8 | 140 | 接触摩擦（Q8.8） |
| ACCEL_LP_ALPHA | 24 | IMU 低通 alpha（Q8.8） |
| SHAKE_THRESHOLD | 6000 | 晃动能量阈值（原始 LSB） |
| SHAKE_GAIN_Q8 | 10 | 晃动冲量增益（Q8.8） |
| IMU_SWAP_XY / INVERT_X / INVERT_Y | n | 真机轴映射标定 |
| STATS_INTERVAL_MS | 2000 | 周期统计日志 |

## 待办 / 真机验证

- 真机标定 IMU 轴映射与符号（Kconfig 三个开关），确认倾斜方向与方块滑动方向一致
- 真机调参：重力强度、阻尼、恢复系数、晃动阈值/增益
- 测量物理/渲染耗时、实际 FPS、CPU/PSRAM 占用并记录到 [dev-log.md](dev-log.md)
