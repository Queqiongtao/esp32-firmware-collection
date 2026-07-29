# PrintSphere for ESP32-P4 JC1060P470

这是 PrintSphere `v1.6.2` 在 JC1060P470（ESP32-P4 + ESP32-C6）上的固定快照迁移。业务基线来自上游标签 `v1.6.2`（提交 `32ad5aeb8b5da999de9d94bd9f4d2ec0f065c609`），显示、触摸、音频和 Hosted Wi-Fi 板级实现来自本仓库 `esp32p4/JC1060P470`。

当前工程已在 ESP-IDF `6.0.1` 下完成编译和链接验证。显示、触摸、ESP-Hosted、音频、电池采样、打印机连接及 OTA 均仍需在目标板上逐项验收。

## 固定技术基线

- 目标：ESP32-P4 V1.0、Hex PSRAM、16 MiB Flash
- 显示：JD9165、MIPI DSI 2 lane、1024x600 RGB565、约 60 Hz
- 触摸：GT911，共享 I2C1
- 网络：板载 ESP32-C6，经 SDIO Slot 1 使用 ESP-Hosted/Wi-Fi Remote
- UI：LVGL 9.5.0 + esp_lvgl_adapter，原生 1024x600 横屏，仅支持 0°/180°
- 音频：ES8311 + I2S0
- 电池：GPIO52 / ADC2 Channel 3 分压采样
- 许可证：保留上游 FNCL 1.1，仅用于个人非商业用途

## 构建

```bash
cd esp32p4/PrintSphere
./dev.sh build
./dev.sh flash -p /dev/ttyUSB0
./dev.sh monitor -p /dev/ttyUSB0
```

`dev.sh` 通过 `eim` 选择当前环境，依赖清单固定要求 ESP-IDF `6.0.1`。依赖解析结果提交在 `dependencies.lock`。

## 文档

- [迁移总计划](docs/plans/00_master_plan.md)
- [详细实施计划](docs/plans/01_implementation_breakdown.md)
- [系统架构](docs/architecture/system_architecture.md)
- [技术框架与依赖](docs/architecture/technology_stack.md)
- [网络与应用协议](docs/protocols/network_protocols.md)
- [硬件适配](docs/hardware/jc1060p470.md)
- [测试与验收](docs/testing/validation_plan.md)

## 发布与验证边界

`partitions.csv` 使用双 5 MiB OTA 槽，并保留约 4.94 MiB `sounds` LittleFS 分区。OTA 仅接受 `project_name=printsphere_p4` 的应用镜像，并启用关键启动失败自动回滚。`release_initial_flash` 可生成初始整包和 OTA 镜像，但在首次上板完成分区、回滚和断电恢复测试前，不应发布给其他设备使用。
