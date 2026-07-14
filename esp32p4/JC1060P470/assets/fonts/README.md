放置 UI 运行时字体文件。

当前实现默认从独立 `font` SPIFFS 分区加载思源黑体简中 Regular 的子集字体：

- 源字体：Source Han Sans SC Regular / 思源黑体简中 Regular
- 源仓库：<https://github.com/adobe-fonts/source-han-sans>
- 许可证：SIL Open Font License 1.1
- 文件名：`ui_shs_sc.otf`
- 挂载点：`/spiffs`
- 运行时路径：`/spiffs/ui_shs_sc.otf`
- 当前分区大小：`2M`
- 运行时后端：`LVGL FreeType`

字符集生成规则：

- 扫描 `main/*.c`、`main/*.h` 中的 C 字符串字面量
- 固定保留全部可打印 ASCII
- 额外补集来自 `ui_font_extra_chars.txt`，优先覆盖常见 WiFi SSID / 设备名
- 生成的合并字符集会写入 `ui_font_charset.txt`

生成方法：

```bash
python3 -m pip install fonttools
python3 assets/fonts/build_subset.py --source /path/to/SourceHanSansSC-Regular.otf
```

不要提交完整官方源字体；仓库只保留生成后的 UI 子集字体。若重新生成或更换源字体，
同步保留/更新对应许可证说明，避免把子集文件误标为官方完整原版。

静态 UI 字库：

- 普通 UI 使用 `main/lvgl_demo_font_patch.c` 的 16 px 静态字库，不依赖 FreeType。
- FreeType 仅用于 Demo 中的字体缩放测试样本。
- 修改 UI 文案后，使用当前字符集和思源黑体源文件重新生成静态字库：

```bash
npx lv_font_conv --size 16 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name lvgl_demo_font_patch_16 \
  --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf \
  --symbols "$(tr -d '\n' < assets/fonts/ui_font_charset.txt)" \
  --output main/lvgl_demo_font_patch.c
```

构建与回退：

- 构建时通过 `spiffs_create_partition_image(font ../assets/fonts FLASH_IN_PROJECT)` 打包进 `font` 分区
- 若字体文件缺失、分区未烧录或 FreeType 初始化失败，程序会回退到内置 `lvgl_demo_font_patch_16`
- 仓内正式资源是子集字体，原始完整字体不作为运行时资源保留
