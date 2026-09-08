# ESP32-S3 电路板信息报告

> 检测时间：2026-08-26 13:16 CST（Asia/Shanghai）  
> 最近更新：2026-08-26 14:16 CST  
> 检测方式：USB 串口日志 + `esptool` 只读查询  
> 检测过程中未擦除或烧录 Flash；查询完成后板卡已自动复位并恢复运行。

## 1. 检测结论

当前连接的电路板使用 **ESP32-S3** 芯片，实测配置为 **16 MB Flash + 8 MB PSRAM**。芯片支持 2.4 GHz Wi-Fi、Bluetooth 5 LE、双核处理器以及面向 AI/DSP 的向量指令。

| 项目 | 串口实测结果 |
| --- | --- |
| 芯片 | ESP32-S3 |
| 封装 | QFN56 |
| 芯片修订版 | v0.2 |
| CPU | 双核 Xtensa LX7，最高 240 MHz；另有 ULP 协处理器 |
| 晶振 | 40 MHz |
| 无线能力 | 2.4 GHz Wi-Fi、Bluetooth 5 LE |
| Flash | 16 MB，Quad SPI，3.3 V |
| PSRAM | 8 MB，芯片内封装 PSRAM（AP_3v3） |
| MAC 地址 | `b8:1f:3f:c4:71:c0` |
| Secure Boot | 未启用 |
| Flash Encryption | 未启用 |
| 当前应用空闲 SRAM | 约 170–178 KB（随运行状态变化） |

> ESP32-S3 不提供传统的独立 Chip ID，`esptool` 会使用基础 MAC 地址作为设备身份标识。

## 2. 串口连接信息

| 项目 | 检测结果 |
| --- | --- |
| 串口设备 | `/dev/cu.usbserial-1141240` |
| USB 转串口芯片 | WCH CH340/CH341 系列 |
| USB VID:PID | `1A86:7523` |
| 日志波特率 | 115200 baud |
| 数据格式 | 8 数据位、无校验、1 停止位（8N1） |

CH340/CH341 只是 USB-UART 桥接芯片，并不代表具体开发板型号。串口能够确认 MCU 和存储配置，但无法仅凭这些信息确定 PCB 厂商或开发板商品名称。

## 3. 固件运行状态

板载程序约每 10 秒输出一次内存状态。读取到的日志示例：

```text
I (480311) SystemInfo: free sram: 177883 minimal sram: 170211
I (490311) SystemInfo: free sram: 173671 minimal sram: 170211
```

完成芯片查询并复位后，再次读取到 ESP32-S3 ROM 启动信息和应用日志：

```text
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
I (30311) SystemInfo: free sram: 178203 minimal sram: 169899
```

由此可以确认：

- 串口通信正常；
- 主程序已在查询后恢复运行；
- 最新样本的空闲 SRAM 为 178,203 字节，历史最低空闲值为 169,899 字节；
- 日志里的空闲 SRAM 是应用运行时余量，不等同于芯片的 SRAM 总容量。

## 4. ESP32-S3 通用规格

以下是 ESP32-S3 芯片系列的通用能力。部分引脚、外设和存储容量会因模组及开发板设计而变化。

### 4.1 处理器与 AI 加速

- 两个 32 位 Xtensa LX7 主处理器核心，最高主频 240 MHz；
- 支持 ULP-RISC-V 与 ULP-FSM 两种低功耗协处理机制，可在主 CPU 休眠时执行简单任务；二者不能同时运行，也不是第三个通用应用 CPU；
- 支持 SIMD/向量指令，可加速神经网络推理、数字信号处理、图像和音频运算，但没有独立 NPU；
- 适合语音唤醒、离线关键词识别、简单视觉识别和边缘 AI 应用。

### 4.2 片上存储与外部存储

- 384 KB ROM；
- 512 KB 片上 SRAM；
- 16 KB RTC 低功耗 SRAM，包括 8 KB RTC FAST SRAM 和 8 KB RTC SLOW SRAM；
- 支持外接 SPI Flash 和 PSRAM；
- 本次实测板卡配置为 16 MB Flash 和 8 MB PSRAM。

### 4.3 无线通信

- 2.4 GHz IEEE 802.11 b/g/n Wi-Fi，不支持 5 GHz Wi-Fi；
- 支持 Bluetooth 5.0 Low Energy 和 BLE Mesh；
- 可使用 BLE 2 Mbps、Coded PHY/Long Range 等能力，具体取决于固件和软件栈配置；
- 不支持 Bluetooth Classic（经典蓝牙）。

### 4.4 常用接口与外设

- 最多 45 个可编程 GPIO，实际可用数量取决于封装和板卡占用；
- 3 路 UART、2 路 I²C、2 路 I²S，以及 SPI、SD/MMC 等接口；
- USB 2.0 OTG Full-Speed（12 Mbit/s），可作为 Host 或 Device；
- USB Serial/JTAG，可用于下载、日志和调试；
- LCD/Camera 接口，适合显示屏与摄像头应用；
- 12 位 SAR ADC、触摸传感器、温度传感器；
- PWM、RMT、脉冲计数器、TWAI（兼容 CAN 2.0）等外设。

### 4.5 安全能力

ESP32-S3 支持 Secure Boot v2、Flash Encryption（XTS-AES）、eFuse、硬件随机数生成以及 AES、SHA、RSA、HMAC、数字签名等硬件安全功能。

这些功能由芯片提供，但是否启用取决于产品固件和量产配置。本次实测板卡的 Secure Boot 与 Flash Encryption 均处于关闭状态。

## 5. Arduino 开发环境配置

### 5.1 首次检查到的配置

本机 Arduino 环境中检测到的当前配置为：

| 配置项 | 当前设置 | 实测硬件 |
| --- | --- | --- |
| 开发板 | ESP32S3 Dev Module | ESP32-S3 |
| Flash | 4 MB | 16 MB |
| PSRAM | Disabled | 8 MB |
| ESP32 Arduino Core | 3.3.11 | — |

首次检查到的 Flash 与 PSRAM 设置没有充分利用硬件资源。

### 5.2 推荐菜单配置

| Arduino IDE 菜单项 | 推荐设置 | 说明 |
| --- | --- | --- |
| Board | `ESP32S3 Dev Module` | 最匹配当前已确认的信息 |
| CPU Frequency | `240MHz (WiFi)` | 匹配芯片最高工作频率 |
| Flash Mode | `QIO 80MHz` | 实测 Flash 为 Quad 模式 |
| Flash Size | `16MB (128Mb)` | 实测容量为 16 MB |
| PSRAM | `OPI PSRAM` | 实测为 8 MB 内封装 Octal PSRAM |
| Partition Scheme | `16M Flash (3MB APP/9.9MB FATFS)` | 双 3 MB OTA 应用区，并使用完整 16 MB Flash |
| USB Mode | `Hardware CDC and JTAG` | 可保留默认值 |
| USB CDC On Boot | `Disabled` | 当前日志和上传使用外置 CH340 |
| Upload Mode | `UART0 / Hardware CDC` | 匹配当前 CH340 串口连接 |
| Upload Speed | `460800` | 已实测稳定；若仍失败则使用 115200 |
| Erase All Flash Before Sketch Upload | `Disabled` | 普通上传无需擦除整片 Flash |

不要选择 `ESP32S3 Dev Module Octal (WROOM2)`：该板型配置面向 **Octal Flash**，而本板实测为 **Quad Flash + Octal PSRAM**。Flash 与 PSRAM 的总线类型不是同一个设置。

命令行使用的完整 FQBN 示例：

```text
esp32:esp32:esp32s3:UploadSpeed=460800,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default
```

配置时还应注意：

1. 分区表属于应用需求选择，而不是芯片型号判定；上述分区适合需要 OTA 和较大文件区的通用项目；
2. 若不需要 FATFS，可根据应用大小改用其他 16 MB 分区或自定义分区；
3. 修改分区表后重新上传可能改变原有数据区布局，应先备份需要保留的数据；
4. 修改后应通过程序启动日志再次确认 Flash 和 PSRAM 初始化结果。

在未确认模组丝印、原理图或具体开发板型号前，不建议仅依据存储容量断言板卡型号。该硬件表现符合常见的 ESP32-S3 **N16R8** 存储组合，但这不是对具体模组型号的最终确认。

## 6. 编译结果说明

Arduino 编译成功，输出如下：

```text
Sketch uses 293928 bytes (22%) of program storage space.
Maximum is 1310720 bytes.
Global variables use 22616 bytes (6%) of dynamic memory,
leaving 305064 bytes for local variables. Maximum is 327680 bytes.
```

- 程序本体为 293,928 字节，编译阶段没有容量问题；
- `Maximum is 1310720 bytes` 表示当前所选分区只给应用提供约 1.25 MB，符合默认 4 MB 分区布局，并不表示实物 Flash 只有 4 MB；
- 改为推荐的 `16M Flash (3MB APP/9.9MB FATFS)` 后，单个应用分区上限约为 3 MB；
- `Maximum is 327680 bytes` 是 Arduino 对内部动态内存的统计，不包含外部 8 MB PSRAM，因此启用 PSRAM 后这行通常仍显示 327,680 字节。

可在程序中确认 PSRAM 是否初始化成功：

```cpp
Serial.printf("PSRAM total: %u bytes\n", ESP.getPsramSize());
Serial.printf("PSRAM free:  %u bytes\n", ESP.getFreePsram());
```

## 7. 上传失败诊断

### 7.1 错误现象

上传程序在启动 `esptool`、连接 ESP32-S3 并加载 RAM Stub 后失败：

```text
Changing baud rate to 921600...
Changed.
A fatal error occurred: Unable to verify flash chip connection
(No more data to read from the serial port.)
```

这不是程序过大，也没有证据表明 Flash 芯片损坏。失败点是串口切换到 921600 baud 后，主机与 RAM Stub 之间的数据连接中断。

### 7.2 只读实测结果

使用同一串口和同一版本的 `esptool` 执行 `flash-id`：

| 上传波特率 | 结果 |
| --- | --- |
| 921600 | 失败，复现 `Unable to verify flash chip connection` |
| 460800 | 成功，识别到 16 MB Flash |
| 115200 | 成功，识别到 16 MB Flash |

因此可将故障定位为 CH340、USB Hub/线缆、驱动与 921600 baud 组合下的链路稳定性问题，而不是 Flash 接口模式错误。

### 7.3 处理方法

1. 在 Arduino IDE 中选择 `Tools → Upload Speed → 460800`；
2. 关闭占用串口的 Serial Monitor 或其他终端；
3. 重新上传；若仍偶发失败，将 Upload Speed 降为 `115200`；
4. 串口监视器继续使用固件日志所需的 `115200`，它与上传速度是两个独立设置；
5. 如果必须使用 921600，可尝试缩短数据线、绕过 USB Hub 或更换 USB-UART 转接器。

## 8. 复现检测

持续查看应用日志：

```bash
arduino-cli monitor \
  -p /dev/cu.usbserial-1141240 \
  --config baudrate=115200
```

使用本机已安装的 `esptool` 读取硬件信息：

```bash
ESPTOOL=/Users/neilsun/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool
PORT=/dev/cu.usbserial-1141240

"$ESPTOOL" --port "$PORT" chip-id
"$ESPTOOL" --port "$PORT" read-mac
"$ESPTOOL" --port "$PORT" flash-id
"$ESPTOOL" --port "$PORT" get-security-info
```

这些命令只读取芯片信息，但会通过 RTS/DTR 让开发板短暂进入下载模式，并在完成后自动复位。

## 9. 官方资料

- [ESP32-S3 产品页](https://www.espressif.com/en/products/socs/esp32-s3)
- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [ESP-IDF：ESP32-S3 入门文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
