# 触摸输入（FT3168）

最后更新：2026-08-01

本文描述 FT3168 触摸的事件语义与滤波行为。当前方块物理效果（`components/block_physics/`）为纯 IMU 交互、不消费触摸事件；触摸驱动作为板级能力保留，供后续恢复触摸交互时使用。

## 硬件与驱动

- 芯片：FT3168，I2C 地址 `0x38`（100 kHz），中断引脚 GPIO21，复位经 TCA9554 EXIO2
- 驱动路径：`esp_lcd_touch_new_i2c_ft5x06`（兼容路径）+ 本地封装 `board_touch_ft3168`
- 坐标范围：x_max=368，y_max=448（与面板分辨率一致；swap_xy/mirror 均未启用，真机方向需实测）
- 轮询周期：`main/main.cpp` 主循环 10ms；空闲时由中断窗口门控，轮询开销接近零

## 事件语义（`board_touch_read` 输出）

| 字段 | 含义 |
| --- | --- |
| `pressed` | 1=按下，0=抬起 |
| `x, y` | 当前/最近有效坐标 |
| `point_num` | 触摸点数；`BOARD_TOUCH_RELEASE_EVENT_POINT_NUM`（UINT8_MAX）表示一次合成抬起事件 |

## 滤波与去抖参数（`board_touch_ft3168.cpp`）

| 参数 | 值 | 作用 |
| --- | --- | --- |
| 按下确认样本数 | 1 | 一次采样即确认 down |
| 抬起确认样本数 | 2 | 连续两次无点才确认 up |
| 移动确认样本数 | 1 | 移动候选需超过 12 px 才更新 |
| 候选保持窗口 | 90 ms | down 候选有效窗口 |
| 中断窗口 | 90 ms | 进入采样状态的门控 |
| 噪声跳变 | \|dx\|≥64 且 \|dy\|≤6 | 判定为横向跳变噪声，连续 2 次则忽略 |
| 陈旧释放超时 | 180 ms | 有触摸后长时间无数据时合成 up |

## 当前消费方式

`main/main.cpp` 已不再轮询触摸（方块物理效果由 IMU 驱动，见 [block-physics-design.md](block-physics-design.md)）。未来若恢复触摸交互，建议：

1. 新增独立事件源/环形缓冲（如 `touch_events` 队列），板级只提供采样，事件化放在渲染层或应用层
2. `press` -> 在手指位置注入扰动/能量源；`move` -> 沿轨迹施加扰动速度；`up` -> 停止注入
3. 坐标直接使用 368x448 屏幕坐标，渲染层负责映射到物理世界坐标

注意：当前封装只读取单点（`point_data[1]`），FT3168 虽支持多点，但本驱动未启用多点路径；若后续交互需要双指，需扩展读取逻辑。

## 参考

- `components/board_amoled_1_8/board_touch_ft3168.cpp`：全部滤波实现
- `companion_display/firmware/esp32s3/docs/device.md`：同板卡触摸事件化与批量上报的实现参考
