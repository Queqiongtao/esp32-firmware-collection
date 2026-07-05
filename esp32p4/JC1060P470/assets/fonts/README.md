放置 UI 运行时字体文件。

当前实现默认从独立 `font` SPIFFS 分区加载子集 `TTF`：

- 文件名：`NotoSansSC-Regular.ttf`
- 挂载点：`/spiffs`
- 运行时路径：`/spiffs/NotoSansSC-Regular.ttf`
- 当前分区大小：`2M`
- 运行时后端：`LVGL Tiny TTF`

字符集生成规则：

- 扫描 `main/*.c`、`main/*.h` 中的 C 字符串字面量
- 固定保留全部可打印 ASCII
- 额外补集来自 `ui_font_extra_chars.txt`，优先覆盖常见 WiFi SSID / 设备名
- 生成的合并字符集会写入 `ui_font_charset.txt`

生成方法：

```bash
python3 -m pip install fonttools
python3 assets/fonts/build_subset.py --source /path/to/NotoSansSC-Regular.ttf
```

构建与回退：

- 构建时通过 `spiffs_create_partition_image(font ../assets/fonts FLASH_IN_PROJECT)` 打包进 `font` 分区
- 若字体文件缺失、分区未烧录或 Tiny TTF 初始化失败，程序会回退到内置 `lvgl_demo_font_patch_16`
- 仓内正式资源是子集 `TTF`，原始完整字体不再作为运行时资源保留
