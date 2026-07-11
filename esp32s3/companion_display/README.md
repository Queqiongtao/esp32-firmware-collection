# Companion Display

`companion_display` turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 into a small companion display. It mirrors a selected desktop, window, or crop through Wi-Fi or native USB Vendor Bulk. It is not an operating-system extended monitor and needs no display driver.

## Firmware

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

On first boot, connect to the `Companion-XXXX` Wi-Fi AP using the password configured by `CONFIG_COMPANION_AP_PASSWORD` (default: `companion-display`). Open `http://192.168.4.1/`, enter the target Wi-Fi network, then use the printed pairing token with the desktop client. The device logs its DHCP address after joining the LAN.

This version uses the DHCP address directly. mDNS discovery is deferred until the Espressif `mdns` component is added as a project dependency.

USB exposes a Vendor Bulk interface (`303A:4001` by default). Windows 8.1+ binds its built-in WinUSB driver from the Microsoft OS 2.0 descriptor. macOS and Linux use userspace USB access; Linux needs an installed udev permission rule in the desktop package.

## Protocol

Both transports carry an identical little-endian frame: a 24-byte `CDP1` header followed by a full `368x448` JPEG image. The header contains protocol version, sequence, format, image size, payload length and CRC32. Wi-Fi sends one frame per binary WebSocket message; USB writes the same byte sequence to the Bulk OUT endpoint.

Wi-Fi is access-controlled with the pairing token but is not encrypted in this first version. Use a trusted LAN.
