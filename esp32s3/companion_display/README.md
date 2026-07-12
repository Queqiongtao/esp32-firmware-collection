# Companion Display

`companion_display` turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 into a small companion display. It mirrors a selected desktop, window, or crop through Wi-Fi. It is not an operating-system extended monitor and needs no display driver.

## Firmware

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

On first boot, connect to the `Companion-XXXX` Wi-Fi AP using the password configured by `CONFIG_COMPANION_AP_PASSWORD` (default: `companion-display`). Open `http://192.168.4.1/` and enter the target Wi-Fi network. Once connected, the device screen shows its DHCP address and six-digit pairing code; enter both in the desktop client.

This version uses the DHCP address directly. mDNS discovery is deferred until the Espressif `mdns` component is added as a project dependency.

## Protocol

Wi-Fi carries a little-endian frame: a 24-byte `CDP1` header followed by a full `368x448` JPEG image. The header contains protocol version, sequence, format, image size, payload length and CRC32. TCP mode sends each frame in one binary WebSocket message.

The desktop client now keeps WebSocket/TCP as a reliable control channel and can send video over either TCP (default) or negotiated UDP. UDP uses bounded 1200-byte fragments and drops incomplete frames rather than retransmitting them. See [PROTOCOL.md](PROTOCOL.md) for the handshake, UDP packet format, and future touch/text/audio extension points.

Wi-Fi is access-controlled with a six-digit pairing code but is not encrypted in this first version. Use a trusted LAN.

## Desktop Wi-Fi Sender

The cross-platform sender is in `tools/desktop_client`. It captures a monitor, a window, or a configured crop; preserves its aspect ratio with black letterboxing; encodes a `368x448` JPEG; then sends the shared `CDP1` frame over WebSocket.

Install a current Rust toolchain and Node.js, then run:

```bash
cd tools/desktop_client
npm install
npm run tauri:dev
```

Enter the device WebSocket address such as `ws://192.168.1.42/stream`, the six-digit pairing code shown on the device screen, refresh capture sources, then start the stream. The sender caps output at 30 FPS and 100 KiB JPEG frames.
