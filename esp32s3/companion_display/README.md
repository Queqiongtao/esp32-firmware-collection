# Companion Display

`companion_display` turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 into a small companion display. It mirrors a selected desktop, window, or crop through Wi-Fi or native USB Vendor Bulk. It is not an operating-system extended monitor and needs no display driver.

## Firmware

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

On first boot, connect to the `Companion-XXXX` Wi-Fi AP using the password configured by `CONFIG_COMPANION_AP_PASSWORD` (default: `companion-display`). Open `http://192.168.4.1/` and enter the target Wi-Fi network. Once connected, the device screen shows its DHCP address and six-digit pairing code; enter both in the desktop client.

This version uses the DHCP address directly. mDNS discovery is deferred until the Espressif `mdns` component is added as a project dependency.

USB exposes a Vendor Bulk interface (`303A:4001` by default). Windows 8.1+ binds its built-in WinUSB driver from the Microsoft OS 2.0 descriptor. macOS and Linux use userspace USB access; Linux needs an installed udev permission rule in the desktop package.

## Protocol

Both transports carry an identical little-endian frame: a 24-byte `CDP1` header followed by a full `368x448` JPEG image. The header contains protocol version, sequence, format, image size, payload length and CRC32. Wi-Fi sends one frame per binary WebSocket message; USB writes the same byte sequence to the Bulk OUT endpoint.

Wi-Fi is access-controlled with a six-digit pairing code but is not encrypted in this first version. Use a trusted LAN.

## Desktop Wi-Fi Sender

The cross-platform sender is in `tools/desktop_client`. It captures a monitor, a window, or a configured crop; preserves its aspect ratio with black letterboxing; encodes a `368x448` JPEG; then sends the shared `CDP1` frame over WebSocket.

Install a current Rust toolchain and Node.js, then run:

```bash
cd tools/desktop_client
npm install
npm run tauri:dev
```

Enter the device WebSocket address such as `ws://192.168.1.42/stream`, the six-digit pairing code shown on the device screen, refresh capture sources, then start the stream. The sender caps output at 30 FPS and 100 KiB JPEG frames. USB transport will reuse exactly the same `CDP1` bytes in a later client-side adapter.
