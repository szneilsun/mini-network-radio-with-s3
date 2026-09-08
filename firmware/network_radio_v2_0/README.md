# ESP32-S3 网络收音机 2.0.0

这是基于最近完成的 2.0.1 独立源码整理出的精简 2.0.0 工程。`network_radio_v2_0.ino` 已内联所需的项目源码，不依赖其他版本目录。

保留的功能包括：双页面播放器与管理页面、播放恢复、Wi-Fi 自动重连、双 OTA 分区、播放列表持久化、诊断、管理认证、WS2812 状态灯和页面主题。

已移除不再由当前入口调用的旧 V4 测试音/I2S 路径、旧 V4 页面、重复的旧 V10 页面及其路由代码；不会改变当前 2.0 播放和管理功能。

兼容原有访问方式：Setup AP 使用固定密码 `radio-setup`，联网后仍保留；未设置管理密码时管理页和 OTA 可直接使用，设置后使用 `admin` 的 Basic Auth。OTA 上传沿用原有裸 `.bin` 流程，不需要额外的文件长度请求头。

## 分区与升级

分区表使用 16 MiB Flash：OTA 元数据位于 `0x19000`，应用从 `0x20000` 开始，两个应用槽各为 3 MiB，LittleFS 位于 `0x620000`。

根目录构建命令：

```bash
./tools/build-firmware.sh
```

串口写入时使用同一入口；脚本会根据生成的 `flash_args` 将 `boot_app0` 写到 `0x19000`、应用写到 `0x20000`：

```bash
./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
```

生成的裸应用镜像为本目录的 `network-radio-v2.0.0-ota.bin`，可通过管理页面 OTA 上传。请勿上传 16 MiB 的 `.merged.bin`。

本目录刻意不复制编译缓存、历史 OTA 或完整 LittleFS 镜像。已刷设备会沿用现有 LittleFS 台标；新设备在未刷入台标资源时会自动显示文字占位图。
