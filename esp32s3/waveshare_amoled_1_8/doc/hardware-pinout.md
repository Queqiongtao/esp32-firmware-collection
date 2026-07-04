# Waveshare ESP32-S3-Touch-AMOLED-1.8 硬件连接分配表

本文档记录 `Waveshare ESP32-S3-Touch-AMOLED-1.8` 开发板的硬件连接分配关系，供板级驱动开发和外设初始化对照使用。

- `EXIO0` 到 `EXIO7` 表示扩展 IO
- 表格中的空白单元格表示该 GPIO 或 EXIO 在对应栏位下未分配

| ESP32-S3 | LCD | USB | RTC | SD | ES8311 | QMI8658C | SYS | OUT |
| ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| GPIO1 |  |  |  | MOSI |  |  |  |  |
| GPIO2 |  |  |  | SCK |  |  |  |  |
| GPIO3 |  |  |  | MISO |  |  |  |  |
| GPIO4 | QSPI SIO0 |  |  |  |  |  |  |  |
| GPIO5 | QSPI SI1 |  |  |  |  |  |  |  |
| GPIO6 | QSPI SI2 |  |  |  |  |  |  |  |
| GPIO7 | QSPI SI3 |  |  |  |  |  |  |  |
| GPIO8 |  |  |  |  | I2S_DSDIN |  |  |  |
| GPIO9 |  |  |  |  | I2S_SCLK |  |  |  |
| GPIO10 |  |  |  |  | I2S_ASDOUT |  |  |  |
| GPIO11 | QSPI SCL |  |  |  |  |  |  |  |
| GPIO12 | LCD_CS |  |  |  |  |  |  |  |
| GPIO13 | LCD_TE |  |  |  |  |  |  |  |
| GPIO14 | TP_SCL |  | RTC_SCL |  |  | ESP32_SCL |  | ESP32_SCL |
| GPIO15 | TP_SDA |  | RTC_SDA |  |  | ESP32_SDA |  | ESP32_SDA |
| GPIO16 |  |  |  |  | I2S_MCLK |  |  |  |
| GPIO17 |  |  |  |  |  |  |  | GPIO17 |
| GPIO18 |  |  |  |  |  |  |  |  |
| GPIO19 |  | USB_N |  |  |  |  |  | GPIO18 |
| GPIO20 |  | USB_P |  |  |  |  |  |  |
| GPIO21 | TP_INT |  |  |  |  |  |  |  |
| GPIO38 |  |  |  |  |  |  |  |  |
| GPIO39 |  |  |  |  |  |  |  |  |
| GPIO40 |  |  |  |  |  |  |  |  |
| GPIO41 |  |  |  |  |  |  |  |  |
| GPIO42 |  |  |  |  |  |  |  |  |
| GPIO43 |  |  |  |  |  |  |  | U0TXD |
| GPIO44 |  |  |  |  |  |  |  | U0RXD |
| GPIO45 |  |  |  |  | I2S_LRCK |  |  |  |
| GPIO46 |  |  |  |  | PA_CTRL |  |  |  |
| EXIO0 | LCD_RESET |  |  |  |  |  |  |  |
| EXIO1 | DSI_PWR_EN |  |  |  |  |  |  |  |
| EXIO2 | TP_RESET |  |  |  |  |  |  |  |
| EXIO3 |  |  | RTC_INT |  |  |  |  |  |
| EXIO4 |  |  |  |  |  |  | SYS_OUT |  |
| EXIO5 |  |  |  |  |  |  | AXP_IRQ |  |
| EXIO6 |  |  |  |  |  | QMI_INT1 |  |  |
| EXIO7 |  |  |  | SDCS |  |  |  |  |
