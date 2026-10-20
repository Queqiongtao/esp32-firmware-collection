# Companion Display 协作规则

## 工程边界

- 本目录是 Companion Display v3 的产品工程容器，不是统一构建根；桌面客户端和各芯片固件分别使用自己的构建入口。
- `PROTOCOL.md` 和 `desktop_client/` 必须保持硬件无关，不得依赖 `firmware/` 内部源码与构建产物。
- 设备差异必须通过 `hello_ack` 和 `/api/status` 的协商字段表达，不得在客户端写死某块板卡的尺寸或帧上限。
- 本目录不属于 WDP/WebRTC；不得引入 `wireless_display` 的消息或版本号。

## 文档同步

- 修改控制消息、帧格式、传输、超时、安全边界或兼容行为时，必须同步更新 `PROTOCOL.md`。
- 修改客户端构建、界面、采集、编码、使用或排障方式时，必须同步更新 `README.md` 和 `docs/pc-client.md`。
- 修改设备固件构建、配置、板级行为或排障方式时，必须同步更新对应 `firmware/<芯片>/README.md`。
- 用户文档使用中文；协议常量、命令、代码标识和 JSON 字段保持原始拼写。

## Git 提交

- 未经用户明确要求，不要执行 `git commit` 或 `git push`；只完成代码修改、构建验证和结果汇报，等用户确认后再提交。

## 验证

- 前端执行 `npm run build`。
- Rust 后端在 `desktop_client/src-tauri` 执行 `cargo test` 和 `cargo clippy --all-targets`。
- ESP32-S3 固件在 `firmware/esp32s3` 执行 `./dev.sh build`。
- ESP32-P4 固件在 `firmware/esp32p4` 执行 `./dev.sh build`。
