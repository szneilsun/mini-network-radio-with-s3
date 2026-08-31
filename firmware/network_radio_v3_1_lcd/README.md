# ESP32-S3 网络收音机 3.1.0：ST7735R 启动屏

本版本以已烧录的 V3.0.2 为基线，保留其播放器、管理页面、OTA、LittleFS
播放列表和 WS2812 状态灯逻辑，并加入一块 `ST7735R 128×128` SPI LCD。

## LCD 行为

系统上电后，LCD 在 Wi-Fi、网页和音频初始化前依次完整显示：红、绿、蓝、白、黑。
随后停留在黑底 `TFT OK` 页面，右下方的绿色方块每 500 ms 闪烁一次。该闪烁在主循环
中以非阻塞方式更新，不影响音频解码、Wi-Fi 恢复或网页服务。

## 接线

| LCD 信号 | ESP32-S3 GPIO |
| --- | ---: |
| VCC | 3V3 |
| GND | GND |
| CS | GPIO10 |
| SDA / MOSI | GPIO11 |
| SCL / SCLK | GPIO12 |
| BLK | GPIO13 |
| DC | GPIO14 |
| RST | GPIO15 |
| MISO | 不接 |

SPI 固定为 1 MHz。背光按 GPIO13 高电平点亮实现；若实物模块为低电平点亮，修改
`St7735rDisplay::begin()` 中的两次 `digitalWrite(config::kTftBacklight, …)` 即可。

音频保持 `GPIO4/5/6`，板载 WS2812 状态灯保持 GPIO48；不使用 microSD。

## 构建

从仓库根目录执行：

```bash
ESP32_SKETCH_DIR="$PWD/firmware/network_radio_v3_1_lcd" ./tools/build-firmware.sh
```

烧录已连接的设备：

```bash
ESP32_SKETCH_DIR="$PWD/firmware/network_radio_v3_1_lcd" ./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
```

构建继续使用 ESP32-S3 的 16 MiB Flash、双 OTA 应用槽和 8 MiB PSRAM 配置。V3.1.0
保持 V3.0.2 的 NVS 命名空间与播放列表格式，可通过原有管理页面 OTA 升级。
