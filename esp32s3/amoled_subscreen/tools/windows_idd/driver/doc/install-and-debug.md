# 安装与调试

本文说明测试签名驱动包的安装、卸载和基本排错流程。

## 生成安装包

先生成安装包：

```powershell
powershell -ExecutionPolicy Bypass -File .\package-driver.ps1 -Configuration Release -Platform x64
```

脚本会生成三类交付物：

```text
.\dist\driver\x64\Release
.\dist\installer\x64\Release
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.exe
```

推荐给普通测试人员使用 EXE；需要排查时再使用脚本安装包或原始驱动包。

## 使用 EXE 安装

EXE 路径：

```text
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.exe
```

使用方式：

1. 双击 EXE。
2. 接受 Windows UAC 管理员权限提示。
3. 查看打开的控制台窗口。
4. 等待成功或失败弹窗。
5. 控制台最后会停在 `Press any key to close this window.`，方便查看输出。

EXE 内部会：

- 解压脚本安装包到 `%TEMP%\amoled_subscreen_idd_installer_<pid>`
- 生成安装日志 `install.log`
- 执行 `install-driver.ps1`
- 导入测试证书到本机证书库
- 执行 `pnputil /add-driver ... /install`

如果安装失败，先看 EXE 控制台输出和临时目录中的 `install.log`。

## 使用脚本安装包

脚本安装包路径：

```text
.\dist\installer\x64\Release
```

管理员 PowerShell 执行：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\install-driver.ps1"
```

脚本安装包内容：

```text
driver\
install-driver.ps1
install-driver.cmd
uninstall-driver.ps1
README.txt
```

## 手动安装原始驱动包

原始驱动包路径：

```text
.\dist\driver\x64\Release
```

管理员 PowerShell 执行：

```powershell
pnputil /add-driver ".\dist\driver\x64\Release\amoled_subscreen_idd.inf" /install
```

关键文件：

```text
amoled_subscreen_idd.inf
amoled_subscreen_idd.cat
xfz1986_usb_graphic.dll
amoled_subscreen_idd_test.cer
```

INF 当前匹配的硬件 ID：

```text
USB\VID_303A&PID_2987
```

只有设备枚举出的硬件 ID 匹配时，Windows 才会自动绑定该驱动。

## 测试签名要求

当前包使用本地测试证书签名，不是 Microsoft 正式发布签名。

如果 Windows 拒绝测试签名驱动，用管理员 PowerShell 开启测试模式：

```powershell
bcdedit /set testsigning on
```

然后重启 Windows，再重新运行安装器。

如果 Secure Boot 阻止测试模式，需要先在 BIOS/UEFI 中关闭 Secure Boot。

## 证书信任

`package-driver.ps1` 默认会把测试证书导入当前用户：

```text
Cert:\CurrentUser\Root
Cert:\CurrentUser\TrustedPublisher
```

安装脚本和 EXE 安装器会在管理员权限下把测试证书导入本机证书库：

```text
Cert:\LocalMachine\Root
Cert:\LocalMachine\TrustedPublisher
```

## 查看已安装驱动

列出驱动：

```powershell
pnputil /enum-drivers
```

可以在输出中查找：

```text
amoled_subscreen_idd.inf
amoled_subscreen development
AMOLED Subscreen USB Display
```

## 卸载驱动

优先使用脚本安装包里的卸载脚本。

预览匹配项：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1"
```

确认卸载：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1" -Force
```

也可以手动卸载。先找到 `Published Name`，例如：

```text
oem42.inf
```

卸载命令：

```powershell
pnputil /delete-driver oem42.inf /uninstall /force
```

如果设备仍在使用，先在设备管理器中禁用或拔掉设备。

## 设备管理器检查点

安装后重点检查：

- 设备是否在 USB 设备下正常枚举
- 显示适配器或监视器相关条目是否出现
- 设备属性中的驱动提供商是否为 `amoled_subscreen development`
- 是否有黄色感叹号

如果驱动未绑定，优先检查设备硬件 ID 是否为：

```text
USB\VID_303A&PID_2987
```

## 运行时配置

驱动会读取注册表：

```text
HKLM\SYSTEM\CurrentControlset\Services\xfz1986_usb_graphic\Parameters
```

当前支持的配置入口包括：

- `debug_level`
- `scale_res`
- `udisp`

`udisp` 字符串会影响运行时策略，例如分辨率、编码、质量、FPS 和缓冲限制。若注册表未配置，驱动会使用 USB 产品字符串或默认策略。

脚本安装包和 EXE 安装器会默认写入：

```text
esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200
```

这表示：

```text
分辨率: 368x448
编码: JPEG
JPEG 质量: 8
帧率: 30 FPS
单帧缓冲上限: 200 KB
```

如需临时调参，可用管理员 PowerShell 覆盖：

```powershell
reg add "HKLM\SYSTEM\CurrentControlSet\Services\xfz1986_usb_graphic\Parameters" /v udisp /t REG_SZ /d "esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200" /f
```

修改后需要重新插拔设备，或禁用/启用设备，让驱动重新读取配置。

## 常见安装问题

### 双击 EXE 没有弹窗或控制台

请确认使用的是重新生成后的 EXE。新版 EXE 的文件说明不应再是 `Win32 Cabinet Self-Extractor`，运行时应触发 UAC 并打开控制台。

重新生成：

```powershell
powershell -ExecutionPolicy Bypass -File .\package-driver.ps1 -Configuration Release -Platform x64
```

### CAT 签名验证失败

重新生成安装包：

```powershell
.\package-driver.ps1
```

然后验证：

```powershell
signtool verify /v /pa .\dist\driver\x64\Release\amoled_subscreen_idd.cat
```

### Windows 拒绝安装测试驱动

确认：

- 已开启测试模式
- 已重启
- Secure Boot 未阻止测试模式
- CAT 文件已经签名
- INF、CAT、DLL 在同一个安装包目录中

### 设备管理器显示代码 1

如果设备管理器显示：

```text
此设备的配置不正确。 (代码 1)
```

优先确认使用的是重新生成后的安装包。旧包中的 INF 曾包含 `UmdfLibraryVersion=$UMDFVERSION$` 占位符，可能导致 UMDF 服务配置不完整。新包中的 INF 应为：

```text
UmdfLibraryVersion=2.23
UmdfExtensions=IddCx0102
```

处理步骤：

1. 重新生成安装包：

```powershell
powershell -ExecutionPolicy Bypass -File .\package-driver.ps1 -Configuration Release -Platform x64
```

2. 卸载旧驱动包。先预览匹配项：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1"
```

3. 确认删除匹配项：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1" -Force
```

4. 拔插设备，或在设备管理器中卸载该设备并勾选删除驱动程序。
5. 重新双击新版 EXE 安装器，接受 UAC 提权。
6. 安装后再次拔插设备。

如果仍然是代码 1，继续检查设备事件和安装日志：

- EXE 解压目录中的 `install.log`
- 设备管理器 -> 设备属性 -> 事件
- `pnputil /enum-drivers` 中是否仍残留旧的 `amoled_subscreen_idd.inf`

### 设备无法绑定驱动

检查：

- 设备 VID/PID 是否匹配 INF
- INF 是否已经通过 `pnputil` 添加
- 旧版本驱动是否还占用设备
- 设备是否需要重新插拔

### 当前现场诊断要点

如果硬件已经插上但驱动仍报错，先运行只读诊断脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\diagnose-driver.ps1
```

在仓库目录运行时，报告会输出到：

```text
.\dist\diagnostics\diagnose_<timestamp>.txt
```

重点看这些项：

- 当前是否存在 `USB\VID_303A&PID_2987`
- 是否只看到 `USB\VID_303A&PID_1001` 或 `USB\VID_303A&PID_4001`
- `pnputil /enum-drivers` 中是否有多个 `amoled_subscreen_idd.inf`
- 最新 EXE 解压目录里的 `install.log`
- System 日志中是否有 `Microsoft-Windows-DriverFrameworks-UserMode` 的 `10101` 或 `0x57`

当前已观察到的现场特征：

- 安装日志显示驱动包已添加到驱动库，但系统提示包已存在。
- 系统里可能同时残留 `oem77.inf`、`oem78.inf` 两个 `amoled_subscreen_idd.inf`。
- 当前连接设备列表里没有 `USB\VID_303A&PID_2987` 时，Windows 不会自动绑定该 IDD 驱动。
- `USB\VID_303A&PID_1001` 和 `USB\VID_303A&PID_4001` 通常是 ESP32 USB Serial/JTAG 或下载相关模式，不应直接改 INF 去匹配它们，除非固件明确把这些 PID 作为显示接口。

建议顺序：

1. 先确认固件侧枚举出的目标显示接口 VID/PID。
2. 如果目标确实应为 `USB\VID_303A&PID_2987`，但当前没有出现，先查固件 USB 描述符或设备模式。
3. 确认目标 VID/PID 后，再清理旧的 `oem*.inf` 包并重新安装。

### 设备管理器显示代码 31 / CM_PROB_FAILED_ADD

如果设备已经绑定到 `AMOLED Subscreen USB Display`，但设备管理器显示：

```text
此设备无法正常工作，因为 Windows 无法加载这个设备所需的驱动程序。(代码 31)
```

并且 `setupapi.dev.log` 中出现：

```text
Using WDF schema version 2.23 when section requires version 2.25
Device has problem: 0x1f (CM_PROB_FAILED_ADD), problem status: 0xc0000001
```

这类问题通常说明 WUDF 在进入驱动 DLL 之前就失败了。当前已确认本机只注册了 `IddCx0102`，如果 INF 声明 `IddCx0106`，WUDF 会在加载扩展阶段失败，设备表现为 Code 31。当前工程采用与本机 WUDF/IddCx 注册项一致的组合：

```text
UmdfLibraryVersion=2.23
UMDF_VERSION_MINOR=23
UmdfExtensions=IddCx0102
IDDCX_VERSION_MINOR=2
```

处理顺序：

1. 重新运行 `package-driver.ps1` 生成新的 INF/CAT/DLL/EXE。
2. 卸载旧的 `amoled_subscreen_idd.inf` 驱动包。
3. 安装新包后重新插拔设备。
4. 再运行 `diagnose-driver.ps1`，重点查看 `Recent SetupAPI WDF And AMOLED Lines` 和 WUDF 相关事件。
5. 如果仍是 Code 31，启用 `Microsoft-Windows-DriverFrameworks-UserMode/Operational` 日志后重装，再根据 WUDF 事件定位 `AddDevice` 或 `PrepareHardware` 的具体失败点。
