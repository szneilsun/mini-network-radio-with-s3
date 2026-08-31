# ESP32-S3 网络收音机

当前开发版本为 [V3.1.0 LCD](firmware/network_radio_v3_1_lcd/)，新增 ST7735R
128×128 的启动自检与 `TFT OK` 闪烁状态页。

可复现构建的完整源码包位于
[`release/network-radio-v3.1.0-lcd-source/`](release/network-radio-v3.1.0-lcd-source/)。
它包含 Arduino 工程、分区表、113 个 LittleFS 台标、编译脚本和固定版本的
`ESP32-audioI2S` 源码依赖。

## 在新目录中编译

安装 `arduino-cli` 与 ESP32 Arduino Core 3.3.11：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
```

然后在仓库根目录执行：

```bash
cd release/network-radio-v3.1.0-lcd-source
./build.sh
```

脚本会生成 OTA、LittleFS 与完整 16 MiB 恢复镜像。生成文件不纳入 Git；
详情见该目录的 README。
