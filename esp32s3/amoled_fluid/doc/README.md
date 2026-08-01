# 技术文档索引

本目录记录 `amoled_fluid` 工程的技术文档。修改行为、新增能力或实验测量后，必须同步更新对应文档。

| 文档 | 内容 | 主要读者 |
| --- | --- | --- |
| [hardware-pinout.md](hardware-pinout.md) | Waveshare ESP32-S3-Touch-AMOLED-1.8 板卡 GPIO/EXIO 硬件连接全表 | 板级开发、接线排查 |
| [board-bringup.md](board-bringup.md) | 板级启动时序：I2C、EXIO、PMU、LCD、触摸的初始化顺序与关键延迟 | 板级开发、启动排障 |
| [display-pipeline.md](display-pipeline.md) | 显示链路与性能预算：SH8601 参数、QSPI 带宽、帧缓冲策略、字节序坑 | 渲染/效果开发 |
| [touch-input.md](touch-input.md) | FT3168 触摸事件语义、滤波去抖、坐标映射与后续交互预留 | 交互开发 |
| [fluid-effect-design.md](fluid-effect-design.md) | 流体效果方案设计：形态定义、实现路线、性能约束、待讨论问题 | 产品与渲染开发 |
| [dependencies-build.md](dependencies-build.md) | 组件依赖、构建/烧录流程与 sdkconfig 关键配置 | 所有人 |
| [dev-log.md](dev-log.md) | 按日期记录开发决策、测量数据与踩坑 | 所有参与者 |

## 相关外部参考

- `companion_display/firmware/esp32s3/docs/display-color-format.md`：同款 SH8601 面板 RGB565 字节序实测结论，本工程渲染层必须引用
- `companion_display/firmware/esp32s3/docs/device.md`：同板卡设备端实现参考（显示链路、触摸事件化模型）
- `esp32s3/amoled_subscreen/`：同板卡 USB 副屏工程，展示“复用板级组件 + 独立业务组件”的工程组织先例
- `esp32p4/JC1060P470/docs/display_solution.md`：显示方案参数化文档模板

## 文档维护规则

- 每篇文档标明“最后更新日期”
- 新结论（实测数据、踩坑）必须当天写入 `dev-log.md`，再择机回写正文
- 引用组件源码的结论需注明出处（组件名 + 版本），避免凭记忆
