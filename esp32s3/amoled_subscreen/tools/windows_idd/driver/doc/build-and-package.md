# 构建与打包

本文说明如何从源码构建 Windows IDD 驱动，并生成原始驱动包、脚本安装包和可双击运行的 EXE 安装器。

## 环境要求

需要安装：

- Visual Studio 2026 或兼容的 MSBuild
- Windows Driver Kit
- Windows SDK 10.0.28000.0 或兼容版本
- PowerShell
- .NET Framework C# 编译器 `csc.exe`（Windows 或 Visual Studio 通常自带）

当前工程使用：

- `WindowsUserModeDriver10.0`
- UMDF 2.23
- IddCx 1.2
- x64 平台

本地 JPEG 静态库位于：

```text
.\lib\jpeg-static.lib
```

该库当前是 x64，因此主流程只支持 `x64`。

## 直接构建 DLL

命令行构建 `Release|x64`：

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  .\idd_xfz1986_usb_graphic.vcxproj `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /m:1 `
  /v:minimal
```

输出 DLL：

```text
.\x64\Release\xfz1986_usb_graphic.dll
```

只构建 DLL 还不是完整安装包。驱动安装包还需要 INF 和 CAT：

```text
amoled_subscreen_idd.inf
amoled_subscreen_idd.cat
xfz1986_usb_graphic.dll
```

## 生成安装包

推荐使用仓库内脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\package-driver.ps1 -Configuration Release -Platform x64
```

脚本会执行：

- 调用 MSBuild 构建 `Release|x64`
- 创建 `dist\driver\x64\Release`
- 复制 INF 和 DLL
- 运行 `Inf2Cat.exe` 生成 CAT
- 创建或复用当前用户的测试代码签名证书
- 使用 `signtool.exe` 签名 CAT
- 生成脚本安装包、zip 包和 EXE 安装器
- 在脚本/EXE 安装流程中写入默认运行时配置 `esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200`

## 输出物

原始驱动包：

```text
.\dist\driver\x64\Release
.\dist\amoled_subscreen_idd_driver_raw_x64_Release.zip
```

内容：

```text
amoled_subscreen_idd.inf
amoled_subscreen_idd.cat
xfz1986_usb_graphic.dll
amoled_subscreen_idd_test.cer
```

脚本安装包：

```text
.\dist\installer\x64\Release
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.zip
```

内容：

```text
driver\
install-driver.ps1
install-driver.cmd
uninstall-driver.ps1
README.txt
```

EXE 安装器：

```text
.\dist\amoled_subscreen_idd_driver_installer_x64_Release.exe
```

## EXE 安装器实现

EXE 由 `package-driver.ps1` 动态生成 C# bootstrapper，不再使用 IExpress 作为主安装器。

生成流程：

- 将脚本安装包压缩为 `driver_payload.zip`
- 生成 `InstallerBootstrapper.cs`
- 生成带 `requireAdministrator` 的 manifest
- 用 `csc.exe` 编译 EXE，并把 payload zip 作为嵌入资源
- 用测试证书签名 EXE

运行行为：

- 双击 EXE 后 Windows 会弹出 UAC 提权确认
- 安装器打开控制台窗口，显示解压路径、日志路径和 `pnputil` 输出
- 安装器把 payload 解压到 `%TEMP%\amoled_subscreen_idd_installer_<pid>`
- 安装器执行解压后的 `install-driver.ps1`
- 安装脚本写入默认 `udisp` 运行时配置，提升默认 JPEG 画质
- 成功或失败都会弹窗提示，控制台结束前会停住等待按键

## 手动安装

原始驱动包可用于设备管理器或 `pnputil` 手动安装：

```powershell
pnputil /add-driver ".\dist\driver\x64\Release\amoled_subscreen_idd.inf" /install
```

原始驱动包只包含 INF、CAT、DLL 和测试证书，不会自动写入运行时画质配置。若使用原始包手动安装，需要额外写入：

```powershell
reg add "HKLM\SYSTEM\CurrentControlSet\Services\xfz1986_usb_graphic\Parameters" /v udisp /t REG_SZ /d "esp32s3udisp0_R368x448_Ejpg8_Fps30_Bl200" /f
```

脚本安装包可用管理员 PowerShell 安装：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\install-driver.ps1"
```

卸载预览：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1"
```

确认卸载：

```powershell
powershell -ExecutionPolicy Bypass -File ".\dist\installer\x64\Release\uninstall-driver.ps1" -Force
```

## 脚本参数

默认命令：

```powershell
.\package-driver.ps1
```

等价于：

```powershell
.\package-driver.ps1 -Configuration Release -Platform x64
```

跳过构建：

```powershell
.\package-driver.ps1 -SkipBuild
```

跳过签名：

```powershell
.\package-driver.ps1 -SkipSign
```

将测试证书导入本机证书库需要管理员 PowerShell：

```powershell
.\package-driver.ps1 -TrustLocalMachine
```

## 签名验证

手动验证 CAT：

```powershell
signtool verify /v /pa .\dist\driver\x64\Release\amoled_subscreen_idd.cat
```

手动验证 EXE：

```powershell
signtool verify /v /pa .\dist\amoled_subscreen_idd_driver_installer_x64_Release.exe
```

## 常见问题

### 双击 EXE 没有反应

旧版包使用 IExpress 自解压，权限不足或批处理窗口闪退时容易表现为没有反应。新版 EXE 是 C# bootstrapper，带 UAC manifest，双击后应出现 UAC、控制台窗口和最终结果弹窗。

### Windows 拒绝测试签名驱动

当前包使用本地测试证书，不是 Microsoft 正式发布签名。必要时用管理员 PowerShell 开启测试模式：

```powershell
bcdedit /set testsigning on
```

然后重启 Windows，再运行安装器。如果 Secure Boot 阻止测试模式，需要先在 BIOS/UEFI 中关闭 Secure Boot。

### LNK4098 运行库冲突警告

当前构建可能出现：

```text
LNK4098: 默认库 LIBCMT 与其他库的使用冲突
```

该警告通常来自 `jpeg-static.lib` 的运行库选项与工程 `/MD` 不完全一致。当前已确认不阻塞 DLL 构建和驱动包生成。后续如需清理该警告，需要重新构建 JPEG 静态库，使运行库选项与工程一致。
