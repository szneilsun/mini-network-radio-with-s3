# ESP32-S3 网络收音机 3.2.1 状态屏源码包

这是 V3.2.1 的可独立编译发布包。它在 LCD 启动自检之外，提供 ST7735R 128×128
纯状态屏、中文电台名与台标显示，不依赖额外显示库。

## LCD 启动行为与接线

上电后依次显示红、绿、蓝、白、黑，再停留于 `TFT OK` 页面并每 500 ms
闪烁绿色指示块。网络服务启动后切换为无触摸的状态页面：64×64 当前台标、居中的
中文电台名（过长时跑马灯）和音量条。屏幕按 180° 安装方向绘制。

| 信号 | ESP32-S3 |
| --- | --- |
| LCD VCC / GND | 3V3 / GND |
| CS / MOSI / SCLK | GPIO10 / GPIO11 / GPIO12 |
| BLK / DC / RST | GPIO13 / GPIO14 / GPIO15 |

SPI 固定为 1 MHz；LCD 的 MISO 不接。音频保持 MAX98357A 的 GPIO4/5/6。

## 内容与构建

- `network_radio_v3_2_status_lcd/`：完整 Arduino 工程、16 MiB 分区表与 113 个台标；
- `libraries/ESP32-audioI2S/` 与 `libraries/TJpg_Decoder/`：固定的音频与 JPEG 台标解码库源码；
- `build.sh`：生成 OTA、LittleFS 和完整 16 MiB 镜像。

需要 `arduino-cli` 和 ESP32 Arduino Core 3.3.11：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
./build.sh
```

输出写入 `binaries/`。将本目录整体提交至 GitHub 即可备份并在另一台电脑重新构建。
