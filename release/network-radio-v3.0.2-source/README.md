# ESP32-S3 网络收音机 3.0.2 源码包

这是可独立编译的 3.0.2 发布包。管理页面采用 2.0.1 的宽松布局和大按钮风格，播放器及底层逻辑基于 3.0.1，并修复了全新 Flash 首次启动时 LittleFS 播放列表校验失败的问题。

## 包含内容

- `network_radio_v3_0_2/`：完整单文件 Arduino 工程、16 MiB 分区表及 113 个台标。
- `libraries/ESP32-audioI2S/`：编译所需的 ESP32-audioI2S 4.0.0 源码及许可证。
- `build.sh`：编译、生成 OTA/LittleFS/全量镜像，以及可选的整片擦除烧录脚本。
- `binaries/`：已经验证的 3.0.2 发布镜像。
- `SHA256SUMS`：发布镜像的完整性校验值。

Arduino-ESP32 自带的 Arduino、Wi-Fi、WebServer、Preferences、LittleFS、ESPmDNS、DNSServer 和 Update 库不重复打包。

## 环境要求

- `arduino-cli`
- `esp32:esp32` 3.3.11
- macOS 或 Linux Bash 环境

安装 ESP32 平台：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
```

## 编译

```bash
chmod +x build.sh
./build.sh
```

输出位于 `binaries/`：

- `network-radio-v3.0.2-ota.bin`：管理页面 OTA 使用。
- `network-radio-v3.0.2-littlefs.bin`：台标资源，写入地址 `0x620000`。
- `network-radio-v3.0.2-full.bin`：完整 16 MiB 镜像，从地址 `0x0` 写入。

## 整片擦除并烧录

关闭所有串口监视器，然后执行：

```bash
./build.sh --flash /dev/cu.usbserial-XXXX
```

该操作会清空 Wi-Fi、电台顺序、音量、主题和管理密码。首次启动后连接热点 `Radio-XXXX`，密码为 `radio-setup`，浏览器打开 `http://192.168.4.1`。

## 硬件配置

- ESP32-S3 N16R8
- MAX98357A：BCLK GPIO4、LRCLK GPIO5、DIN GPIO6
- 板载 WS2812B：GPIO48
- 16 MiB Flash、8 MiB PSRAM
