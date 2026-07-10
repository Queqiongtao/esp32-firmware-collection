# Windows IDD Driver

This test-signed development driver binds to `USB\VID_303A&PID_2987` and
registers this Windows display mode:

- `368x448 @ 30 Hz` (direct AMOLED output)

## Build

Build `driver/idd_xfz1986_usb_graphic.vcxproj` as `Release|x64` from a Visual
Studio Developer Command Prompt with the Windows 10/11 WDK installed. The
project requires the IDDCX include path supplied by the WDK.

## Test Installation

Enable Windows test-signing mode, install the development certificate used to
sign the generated CAT/DLL package, and install `amoled_subscreen_idd.inf`.
Remove any existing driver bound to `USB\VID_303A&PID_2987` before installing
this package. Reconnect the device after installation.

This package is for development only. Product use requires a product-owned
VID/PID and production driver signing.
