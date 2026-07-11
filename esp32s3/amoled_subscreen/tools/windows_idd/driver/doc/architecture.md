# 技术架构

本文概述 Windows IDD 驱动工具的技术原理和主要运行链路。

## 总体结构

该工具是一个 Windows UMDF 用户态驱动，使用 IddCx 实现间接显示设备。

核心链路：

```text
Windows 桌面合成
  -> IddCx swapchain
  -> Driver.cpp 获取帧
  -> D3D11 staging texture 映射到 CPU
  -> 编码为 JPEG 或 RGB 数据
  -> USB Bulk OUT 发送到 ESP32-S3 小副屏
```

驱动不是普通应用程序。它由 Windows Driver Framework 加载，并通过 INF 绑定到 USB 设备。

## INF 绑定关系

INF 文件：

```text
amoled_subscreen_idd.inf
```

当前匹配硬件 ID：

```text
USB\VID_303A&PID_2987
```

关键配置：

- `Class=Display`
- `UpperFilters=IndirectKmd`
- `UmdfDispatcher=NativeUSB`
- `UmdfService=xfz1986_usb_graphic`
- `UmdfExtensions=IddCx0102`

`IndirectKmd` 让 Windows 将该设备接入间接显示栈。`UmdfDispatcher=NativeUSB` 允许 UMDF 驱动直接访问 USB。

## 驱动初始化

入口函数：

```text
DriverEntry
```

主要工作：

- 读取基础注册表配置
- 初始化 WDF driver config
- 注册 `IddSampleDeviceAdd`

设备添加回调：

```text
IddSampleDeviceAdd
```

主要工作：

- 注册 PnP/Power 回调
- 设置 `EvtDevicePrepareHardware`
- 配置 IddCx 回调
- 创建 WDF device
- 创建 `IndirectDeviceContext`
- 创建设备接口
- 调用 `IddCxDeviceInitialize`

`IndirectDeviceContext` 必须早于 `IddCxDeviceInitialize` 创建，避免后续 PnP/IddCx 回调进入时访问未初始化的设备上下文。

## USB 初始化

USB 初始化在：

```text
idd_usbdisp_evt_device_prepareHardware
```

主要工作：

- 创建 `WDFUSBDEVICE`
- 获取 USB 设备信息
- 选择 interface
- 枚举 configured pipes
- 找到 Bulk IN 和 Bulk OUT pipe
- 读取 USB 产品字符串
- 根据 USB 字符串、注册表配置或默认配置决定运行参数

当前主要输出链路使用 Bulk OUT pipe。

## 显示适配器和显示器

`IndirectDeviceContext::InitAdapter` 会初始化 IddCx adapter。

`IndirectDeviceContext::FinishInit` 会创建 monitor。

显示模式来自 `monitor_modes` 和默认 mode 列表。驱动会向系统报告可用分辨率和刷新率，Windows 再为该显示器创建 swapchain。

## Swapchain 帧处理

Windows 给驱动分配 swapchain 后，会调用：

```text
IddSampleMonitorAssignSwapChain
```

随后创建：

```text
SwapChainProcessor
```

`SwapChainProcessor` 内部启动工作线程，核心循环在：

```text
SwapChainProcessor::RunCore
```

处理流程：

1. 调用 `IddCxSwapChainSetDevice`
2. 循环调用 `IddCxSwapChainReleaseAndAcquireBuffer`
3. 获取 `IDXGIResource`
4. 转为 `ID3D11Texture2D`
5. 创建 staging texture
6. 使用 `Map` 读取像素到 `fb_buf`
7. 调用编码器生成传输数据
8. 通过 USB 异步发送
9. 调用 `IddCxSwapChainFinishedProcessingFrame`

## 编码策略

运行时编码器由以下函数选择：

```text
SwapChainProcessor::decision_runtime_encoder
```

当前支持：

- `enc_jpg`
- `enc_rgb565`
- `enc_rgb888a`

配置来源：

- USB 产品字符串
- 注册表 `udisp`
- 默认值

配置解析函数：

```text
parse_usb_dev_info
```

典型参数包括：

- 分辨率
- 编码类型
- JPEG 质量
- FPS
- buffer limit

## USB 发送

发送相关代码在：

```text
usb_transf.cpp
```

关键函数：

- `usb_transf_init`
- `usb_send_msg_async`
- `usb_send_msg_sync`
- `EvtRequestWriteCompletionRoutine`

驱动维护一个 URB item list。每帧编码后从 list 中取出可用 item，将数据填入 `urb_msg`，再通过 `WdfUsbTargetPipeFormatRequestForWrite` 和 `WdfRequestSend` 异步发送。

如果发送数据刚好对齐 endpoint max packet size，代码会补发一个 zero-length packet，用于标识传输结束。

## IOCTL 调试接口

IOCTL 定义在：

```text
ioctl.h
```

当前包含：

- `IOCTL_UDISP_CONFIG`
- `IOCTL_UDISP_ISSUE_URB`
- `IOCTL_UDISP_EXIT_DBG`

IddCx 设备 IO control 回调为：

```text
udisp1986_EvtIoDeviceControl
```

用途包括读取驱动配置、进入调试发送模式、退出调试模式。

## 当前限制

- 主路径只验证了 x64
- 安装包使用测试签名
- JPEG 静态库运行库选项可能引发 `LNK4098` 警告
- PREfast 和 WDK 头文件警告尚未系统清理
- 正式分发仍需要 Microsoft 签名流程
