# Windows IDD 驱动工具文档

本文档目录记录 `tools/windows_idd/driver` 中 Windows 间接显示驱动工具的构建、打包、安装和实现概要。

当前驱动定位：

- Windows UMDF 2 用户态驱动
- 使用 IddCx 注册间接显示适配器和显示器
- 通过 USB Bulk OUT 将桌面帧编码后发送给 AMOLED 小副屏设备
- 当前主目标平台为 `x64 Release`

## 推荐阅读顺序

1. [构建与打包](build-and-package.md)
2. [安装与调试](install-and-debug.md)
3. [技术架构](architecture.md)
4. [源码索引](source-map.md)

## 当前安装包位置

运行 `package-driver.ps1` 后，会生成三类交付物。

原始驱动包：

```powershell
.\dist\driver\x64\Release
```

应包含：

```text
amoled_subscreen_idd.inf
amoled_subscreen_idd.cat
xfz1986_usb_graphic.dll
amoled_subscreen_idd_test.cer
```

脚本安装包：

```powershell
.\dist\installer\x64\Release
```

包含原始驱动包和安装/卸载脚本：

```text
driver\
install-driver.ps1
install-driver.cmd
uninstall-driver.ps1
README.txt
```

压缩包：

```text
.\dist\amoled_subscreen_idd_driver_raw_x64_Release.zip
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.zip
```

EXE 安装器：

```text
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.exe
```

推荐给测试人员使用 EXE 安装器：双击后接受 UAC 提权，控制台会显示安装过程，结束时会弹窗提示结果。

脚本安装包和 EXE 安装器会自动写入默认画质配置：

```text
esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200
```

也可以使用脚本安装包。管理员 PowerShell 执行：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\install-driver.ps1"
```

原始驱动包也可以手动安装：

```powershell
pnputil /add-driver ".\dist\driver\x64\Release\amoled_subscreen_idd.inf" /install
```

原始驱动包手动安装不会自动写入上述运行时配置，需要单独写注册表或改用脚本/EXE 安装包。

说明：Windows 驱动的标准交付物是 INF、CAT 和驱动二进制。EXE 是外层安装引导程序，内部仍会解压这些文件、导入测试证书并调用 `pnputil` 安装。
