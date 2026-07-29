# 系统架构

## 分层

```text
Application / 状态调度
├── ConfigStore / NVS
├── WifiManager / SetupPortal / SerialProvisioner
├── BambuCloudClient ── HTTPS + MQTT/TLS
├── PrinterClient ───── Local MQTT/TLS
├── P1sCameraClient ─── TLS JPEG snapshot
├── StatusResolver ──── Cloud + Local 状态融合
├── AudioNotifier ───── PCM / LittleFS
├── PmuManager ──────── ADC 电池快照
└── Ui ──────────────── LVGL 页面与交互
          │
          ▼
jc1060p470_bsp
├── JD9165 MIPI DSI / 背光
├── GT911 / 共享 I2C
├── ES8311 / I2S
└── ESP-Hosted / ESP32-C6 SDIO
```

`main/src/application.cpp` 是唯一业务编排入口。BSP 拥有板级资源初始化；业务客户端只消费 ESP-IDF 网络、显示和音频抽象，不直接访问板级总线句柄。

## 启动顺序

1. 初始化 NVS，加载并迁移持久化配置。
2. 设置时区和 P4 电源管理。
3. 初始化 ESP-Hosted，等待 `TRANSPORT_UP`，超时或 failure 时停止启动。
4. 初始化 Wi-Fi Remote、STA/AP、Web Config 和 USB 串行配网。
5. 初始化 ADC 电池管理，挂载 `sounds` LittleFS 并初始化 ES8311。
6. 初始化显示、LVGL 和 UI；若当前镜像处于 pending verify，完成关键启动后确认 OTA 镜像。
7. 根据活动打印机和 SourceMode 启动 Cloud、Local 和 Camera worker。
8. 主循环合并状态、更新 UI、处理省电和通知。

## 状态与并发

- 网络客户端在各自 FreeRTOS worker 中工作，通过快照和互斥量传递状态。
- `StatusResolver` 依据数据新鲜度和 SourceMode 选择 Cloud/Local 字段，UI 不直接解释传输状态。
- LVGL 访问必须持有 BSP 显示锁；锁的毫秒超时由 adapter 原样执行，耗时网络、JPEG 和存储操作不得在 LVGL task 内执行。
- 图片 blob 使用共享所有权；描述符切换前清理 LVGL image cache，PSRAM 保留最小余量。
- Hosted transport 是进程生命周期资源，目前不做运行期 teardown；断链恢复由 ESP-Hosted/Wi-Fi 层负责，需上板验证。

## 持久化与升级

NVS 保存 Wi-Fi、Cloud token、打印机资料、显示、声音和 Web PIN 策略。`sounds` LittleFS 只保存自定义音频。OTA 只写非活动 app 槽，不应擦除 NVS 或 LittleFS。上传和 URL 更新都要求 app descriptor 的 project name 为 `printsphere_p4`；bootloader rollback 在关键启动未完成时回退旧槽，关键启动成功后应用确认新镜像。硬件型号编译定义为 `PRINTSPHERE_HW_VARIANT_JC1060P470`，发布包不可与 S3 或其他 P4 工程混刷。

显示、触摸、共享 I2C 和音频初始化采用板级所有权。共享 I2C 使用引用计数；显示或音频失败时只释放自身取得的引用。显示初始化失败会逆序清理 touch、LVGL adapter、panel、DSI bus、I2C 引用和 DPHY LDO。
