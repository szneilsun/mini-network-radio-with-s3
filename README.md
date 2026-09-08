# ESP32-S3 网络收音机

当前维护版本为 **3.2.1-status-lcd**：ESP32-S3 网络收音机，带管理网页、Wi-Fi
多网络保存、OTA、LittleFS 电台台标与 ST7735R 128×128 状态屏。

## 项目结构

```text
firmware/
  network_radio_v3_2_status_lcd/  当前开发与烧录源码
  network_radio_v3_1_lcd/         LCD 初始版本
  network_radio_v*/               历史版本（保留以便回溯）
tools/                            编译、烧录布局校验工具
diagram/                          接线图
release/                          可独立构建的版本化备份包
build/                            本地构建输出（不纳入 Git）
```

日常开发只修改 `firmware/network_radio_v3_2_status_lcd/`；不要直接修改
`release/` 中的历史副本。每次发布后，再从当前固件目录制作独立源码包。

## 当前版本功能

- ST7735R 128×128，SPI 1 MHz，180° 显示；上电红、绿、蓝、白、黑自检后显示 `TFT OK`；
- 当前电台 JPEG 台标、中文居中电台名、超长名称跑马灯、音量条；
- Wi-Fi 配置页支持扫描、保存多个网络并自动依次尝试连接；
- 网页管理、播放列表、OTA、LittleFS、内置电台和 WS2812 状态灯；
- 不使用 microSD。

详细硬件接线、LCD 行为与升级注意事项见
[当前固件说明](firmware/network_radio_v3_2_status_lcd/README.md)。

## 构建当前版本

需要 `arduino-cli`、ESP32 Arduino Core 3.3.11、`ESP32-audioI2S` 和
`TJpg_Decoder` 1.1.0。根目录的构建脚本默认指向当前版本：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "ESP32-audioI2S"
arduino-cli lib install "TJpg_Decoder@1.1.0"

./tools/build-firmware.sh
```

构建输出在 `build/network_radio_v3_2_status_lcd/`，并会自动校验 16 MiB
Flash 布局。烧录连接的开发板：

```bash
./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
```

## 完整可移植备份

[`release/network-radio-v3.2.1-status-lcd-source/`](release/network-radio-v3.2.1-status-lcd-source/)
是经过独立目录构建验证的源码包，包含固定版本的两项依赖库、113 个台标和 `build.sh`。
在另一台电脑下载或解压后，进入该目录运行 `./build.sh` 即可构建，不依赖本仓库其余内容。

生成的 `build/`、`binaries/` 和固件镜像均不提交到 Git；Wi-Fi 密码、管理员密码和
私有流地址也不得提交。
