# 源码索引

本文按文件说明当前驱动工具的主要职责，便于后续维护。

## 工程与打包

### `idd_xfz1986_usb_graphic.vcxproj`

Visual Studio C++ UMDF 驱动工程。

关键配置：

- `ConfigurationType=DynamicLibrary`
- `PlatformToolset=WindowsUserModeDriver10.0`
- `UMDF_VERSION_MAJOR=2`
- `UMDF_VERSION_MINOR=23`
- `IDDCX_VERSION_MAJOR=1`
- `IDDCX_VERSION_MINOR=2`

x64 配置显式包含：

- WDF UMDF include/lib
- IddCx include/lib
- 本地 `include`
- 本地 `lib`

关键链接库：

- `WdfDriverStubUm.lib`
- `iddcxstub.lib`
- `d3d11.lib`
- `dxgi.lib`
- `ntdll.lib`
- `OneCoreUAP.lib`
- `avrt.lib`
- `jpeg-static.lib`

### `amoled_subscreen_idd.inf`

Windows 驱动安装描述文件。

关键内容：

- 绑定 `USB\VID_303A&PID_2987`
- 复制 `xfz1986_usb_graphic.dll` 到 `%12%\UMDF`
- 注册 UMDF service
- 启用 `IndirectKmd`
- 声明 `UmdfExtensions=IddCx0102`

### `package-driver.ps1`

驱动包生成脚本。

主要职责：

- 查找 MSBuild
- 构建 `Release|x64`
- 创建 `dist\driver\x64\Release`
- 复制 INF 和 DLL
- 调用 `Inf2Cat`
- 创建或复用测试证书
- 使用 `signtool` 签名 CAT
- 生成原始驱动包、脚本安装包和 EXE 安装器
- 复制诊断脚本到安装包

## 驱动主逻辑

### `Driver.cpp`

驱动主体文件。

主要职责：

- `DriverEntry`
- WDF device 创建
- IddCx device 初始化
- adapter 和 monitor 生命周期
- swapchain 分配和释放
- D3D11 帧抓取
- 编码器选择
- IOCTL 回调
- 注册表基础配置读取

重点函数：

- `IddSampleDeviceAdd`
- `IddSampleDeviceD0Entry`
- `IndirectDeviceContext::InitAdapter`
- `IndirectDeviceContext::FinishInit`
- `IndirectDeviceContext::AssignSwapChain`
- `SwapChainProcessor::RunCore`
- `enc_grab_surface`
- `parse_usb_dev_info`
- `SwapChainProcessor::decision_runtime_encoder`

### `Driver.h`

驱动主类型定义。

主要内容：

- `urb_itm_t`
- `fps_mgr_t`
- `Direct3DDevice`
- `SwapChainProcessor`
- `IndirectDeviceContext`
- monitor mode 结构

该文件包含 WDF、USB、IddCx、DXGI、D3D11 和 WRL 相关头文件。

## USB 传输

### `usb_transf.cpp`

USB 初始化和数据发送实现。

主要职责：

- 创建 `WDFUSBDEVICE`
- 获取 USB descriptor
- 读取 USB 产品字符串
- 选择 USB interface
- 查找 Bulk IN/OUT pipe
- 初始化 URB list
- 异步发送帧数据
- 读取注册表 `udisp` 配置
- 根据配置更新运行策略

重点函数：

- `get_usb_dev_string_info`
- `SelectInterfaces`
- `registry_config_dev_info`
- `decision_runtime_policy`
- `idd_usbdisp_evt_device_prepareHardware`
- `usb_transf_init`
- `usb_transf_exit`
- `usb_send_msg_async`
- `EvtRequestWriteCompletionRoutine`

### `usb_transf.h`

USB 传输相关声明。

当前主要暴露：

- `idd_usbdisp_evt_device_prepareHardware`
- `usb_transf_init`
- `usb_transf_exit`

## 编码模块

### `enc_base.cpp` / `enc_base.h`

编码基类和帧头处理。

主要职责：

- 定义公共编码接口
- 组织发送数据格式
- 填充 `udisp_frame_header_t`
- 提供基础 JPEG 发送逻辑

### `enc_jpg.cpp` / `enc_jpg.h`

JPEG 编码实现。

主要职责：

- 基于本地 JPEG 库进行压缩
- 支持质量参数
- 输出可通过 USB 发送的 payload

### `enc_raw_rgb.cpp` / `enc_raw_rgb.h`

RGB 原始格式编码。

主要职责：

- RGB565 编码
- RGB888/RGB888A 编码
- 避免 JPEG 压缩开销，适合调试或低分辨率场景

### `tiny_jpeg.c` / `tiny_jpeg.h`

内置 tiny JPEG 编码实现。

用于轻量 JPEG 编码路径。

## 辅助文件

### `ioctl.h`

定义调试 IOCTL：

- `IOCTL_UDISP_CONFIG`
- `IOCTL_UDISP_ISSUE_URB`
- `IOCTL_UDISP_EXIT_DBG`

### `log.h`

日志宏定义。

日志等级：

- error
- warn
- info
- monitor
- debug

### `Trace.h`

WPP trace 配置遗留文件。

当前主日志路径使用 `log.h`，`Driver.tmh` 不再作为构建依赖。

### `Resource.rc` / `resource.h`

DLL 版本资源。

### `include/` 与 `lib/`

本地 JPEG 头文件和 x64 静态库。

当前工程依赖：

```text
include\jpeglib.h
lib\jpeg-static.lib
```
