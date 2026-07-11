# Windows IDD Driver

This test-signed development driver binds to `USB\VID_303A&PID_2987` and
registers this Windows display mode:

- `368x448 @ 30 Hz` (direct AMOLED output)

## Build

Build `driver/idd_xfz1986_usb_graphic.vcxproj` as `Release|x64` from Visual
Studio or a Visual Studio Developer Command Prompt with the Windows 10/11 WDK
installed. The project must use the WDK user-mode driver toolset
`WindowsUserModeDriver10.0`; do not build it as a normal desktop DLL project
with the regular `v143` toolset. The project requires the WDF and IDDCX include
paths supplied by the WDK.

If Visual Studio reports that a WDK header cannot be opened:

- Confirm the project configuration is `Release|x64`.
- In project properties, confirm `Platform Toolset` is
  `WindowsUserModeDriver10.0`.
- Confirm the Visual Studio Installer has Windows driver development support
  and the WDK Visual Studio integration installed for the Visual Studio
  instance used to build this project.
- Confirm the project can resolve WDK headers such as `wdf.h`, `wdfusb.h`, and
  `iddcx.h`.
- If Visual Studio still uses stale project settings, close Visual Studio,
  delete the local `.vs` directory next to the project, reopen the project, and
  reload it.

This project does not require `wudfwdm.h`. Current WDK installations may not
ship that legacy UMDF header, so the driver includes the standard UMDF 2 WDF
headers directly.

## Test Installation

Enable Windows test-signing mode, install the development certificate used to
sign the generated CAT/DLL package, and install `amoled_subscreen_idd.inf`.
Remove any existing driver bound to `USB\VID_303A&PID_2987` before installing
this package. Reconnect the device after installation.

This package is for development only. Product use requires a product-owned
VID/PID and production driver signing.
