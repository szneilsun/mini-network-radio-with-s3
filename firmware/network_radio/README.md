# ESP32-S3 网络收音机

当前内容是 Step 2：MAX98357A 硬件验收加 Web Server 最小框架。固件以 48 kHz、16-bit、双声道标准 I2S 连续输出低音量 440 Hz 正弦波；MAX98357A 模块按默认硬件设置混合为单声道。

## 接线

| ESP32-S3 N16R8 | MAX98357A |
| --- | --- |
| GPIO4 | BCLK |
| GPIO5 | LRCLK / LRC / WS |
| GPIO6 | DIN |
| GND | GND |
| 独立稳定 5 V | VDD |
| OUT+ / OUT- | 一只 4-8 ohm 扬声器 |

扬声器只能接在 `OUT+` 与 `OUT-` 之间；`OUT-` 不是地。不要从开发板的 3.3 V 引脚给功放供电。

`SD_MODE` 和 `GAIN` 可以保持模块默认悬空状态：模块会混合左右声道并采用 9 dB 增益。不要接 PCM5102A 的 `FMT`、`XSMT` 等 DAC 配置脚；本工程只支持 MAX98357A 的数字输入和 BTL 扬声器输出。

## Arduino 配置

- Board：`ESP32S3 Dev Module`
- CPU Frequency：`240MHz (WiFi)`
- Flash Mode / Size：`QIO 80MHz` / `16MB (128Mb)`
- PSRAM：`OPI PSRAM`
- Partition Scheme：`16M Flash (3MB APP/9.9MB FATFS)`
- Upload Mode / Speed：`UART0` / `460800`

项目根目录的 `partitions.csv` 固化了相同的双 OTA（每个 App 3 MB）与 FFat（9.875 MB）布局，供后续网页资源、播放列表和 OTA 使用。

使用命令行编译：

```text
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=460800,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default firmware/network_radio
```

## Step 2：最小 Web 控制台

烧录后，设备会创建一个 Wi-Fi 热点，名称为 `Radio-` 加设备 MAC 前两字节按既有小端规则生成的编号；本板 MAC 为 `B8:1F:3F:C4:71:C0`，因此热点为 `Radio-1FB8`。密码是 `radio-setup`。连接热点后，在浏览器打开 [http://192.168.4.1](http://192.168.4.1)。

页面每两秒显示一次运行时资源，可开关测试音和重启设备。可直接访问 `GET /api/status` 读取 JSON 状态，使用 `POST /api/audio/test?enabled=1` 或 `enabled=0` 开关测试音。

此热点只用于开发阶段的本地管理；Step 3 将实现路由器配网、保存凭据与受保护的局域网管理。若无声，先断电检查 5 V、共地和 OUT+/OUT- 接线。

`config/network_config.example.h` 只预留了网页配网参数；后续不会将 Wi-Fi 密码写入源码。
