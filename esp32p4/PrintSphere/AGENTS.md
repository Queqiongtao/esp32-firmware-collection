# AI 工作指南

## 项目定位

本工程是 PrintSphere `v1.6.2` 到 JC1060P470 ESP32-P4 的独立迁移工程。业务基线固定，不自动跟随上游；不得修改外部 `/home/queqt/project/esp/PrintSphere` 或兄弟工程 `esp32p4/JC1060P470` 来完成本工程需求。

## 修改规则

- 默认使用中文沟通和 Git 提交信息，提交前缀固定为 `P4-PrintSphere: `。
- 工程固定使用 ESP-IDF `6.0.1`、ESP32-P4、LVGL `9.5.0`；升级必须先更新 `docs/architecture/technology_stack.md` 并单独验证。
- 所有计划、架构、协议、硬件和测试资料放在 `docs/` 对应目录，并与实现同步。
- 板级代码收敛在 `components/jc1060p470_bsp`；业务层不得直接重复初始化显示、I2C、I2S 或 Hosted 资源。
- 屏幕原生布局是 1024x600 横屏，仅支持 0°/180°；不得重新暴露 90°/270° 配置。
- 不修改 `managed_components/`。依赖问题在组件清单、配置或本地适配层解决。
- 不提交 `build/`、`managed_components/`、`release/` 或生成的 `sdkconfig`；必须提交 `dependencies.lock`。

## 验证要求

- 代码修改后运行 `./dev.sh build`。
- 涉及显示、触摸、Hosted、音频、电池或 OTA 的改动必须说明是否完成真机验证。
- 未上板的结论只可写为“静态检查通过”或“编译验证通过”，不得描述为稳定可用。
- 上板结果按 `docs/testing/validation_plan.md` 记录，包括固件版本、串口、供电、步骤、日志和结论。
