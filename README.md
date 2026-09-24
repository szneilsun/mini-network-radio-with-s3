# ESP32-S3 网络收音机

当前版本为 **4.5.1**。项目基于 ESP32-S3 N16R8，实现网络电台播放、手机与桌面网页控制、管理后台、Wi-Fi 自动恢复、OTA、诊断日志、LittleFS 台标和板载 RGB 状态灯。

## 目录结构

```text
sources/esp32-network-radio/  Arduino 源码、分区表和台标资源
docs/                         项目与硬件参考文档
build/                        本地编译中间文件
release/                      正式发布的版本化 ZIP 包
tools/                        编译、烧录和布局校验工具
```

升级版本时直接修改 `sources/esp32-network-radio/`，不要新建版本目录。每次版本变更必须同步更新根目录的 `CHANGELOG.md`。

## 主要功能

- HLS/AAC 网络电台播放、播放列表管理和断线自动恢复；
- 普通播放器 `/` 与管理后台 `/admin`；
- Wi-Fi 扫描、最多保存 5 个网络、启动时自动选择可用网络、配网热点和 mDNS 访问；
- 网页 OTA、诊断日志、管理密码及恢复出厂设置；
- 113 个 LittleFS 电台台标、约 4 秒的开机提示音和 WS2812B 状态指示。

设备联网后访问：

- 播放器：`http://network-radio.local/`
- 管理后台：`http://network-radio.local/admin`

## 硬件与接线

目标开发板为 ESP32-S3 N16R8：16 MiB Quad Flash、8 MiB OPI PSRAM。音频使用 MAX98357A 单声道 I²S 功放。

| ESP32-S3 | MAX98357A | 用途 |
| --- | --- | --- |
| GPIO4 | BCLK | I²S 位时钟 |
| GPIO5 | LRC/LRCLK | I²S 帧时钟 |
| GPIO6 | DIN | I²S 音频数据 |
| GND | GND | 公共地 |
| 5 V 电源 | VIN | 功放供电，需留足扬声器电流 |

两个裸露铜皮触摸电极直接连接 ESP32-S3 原生触摸通道：

| 触摸电极 | ESP32-S3 | 功能 |
| --- | --- | --- |
| 上一台 | GPIO1 / Touch1 | 循环切换到上一个电台 |
| 下一台 | GPIO2 / Touch2 | 循环切换到下一个电台 |

建议使用直径 10–15 mm 的圆形铜皮，电极和走线下方不要铺 GND，走线尽量短。上电时固件会自动校准两个触摸通道，校准期间不要触摸电极。

串口监视器使用 115200 baud 和“新行”或“回车+新行”，可输入以下触摸调试命令：

| 命令 | 功能 |
| --- | --- |
| `touch` / `touch status` | 输出两键的原始值、基线、按下/释放门限和当前状态 |
| `touch on` | 每 500 ms 持续输出触摸状态和按键事件 |
| `touch off` | 停止持续输出 |
| `touch calibrate` | 非阻塞重新校准；执行时须松开两个电极 |
| `touch sensitivity 8` | 设置触发增幅百分比（3–50）；数值越小越灵敏，默认为 8 |
| `help` | 显示可用命令 |

管理后台的“串口日志”区域可分别开关系统/存储、Wi-Fi、音频/播放恢复和触摸按键日志。开关会保存到 NVS 并在重启后保留；关闭只抑制串口输出，下载诊断日志仍保留运行事件。首次升级后默认只开启系统/存储类。

扬声器只能跨接 `OUT+` 与 `OUT-`，任何一端都不能接地。板载 WS2812B 数据脚为 GPIO48。串口下载和日志使用 CH340C 对应的 USB-C 接口，默认波特率为 115200；固件上传建议使用 460800。

状态灯含义：红色呼吸表示连接或缓冲，蓝色呼吸表示正常播放，红色快闪表示网络或播放错误，绿色慢闪表示 OTA，熄灭表示暂停。

## Flash 分区

固件使用 16 MiB Flash 和双 OTA 应用槽：

| 分区 | 偏移 | 大小 | 用途 |
| --- | ---: | ---: | --- |
| NVS | `0x9000` | `0x10000` | Wi-Fi、播放列表及用户设置 |
| OTA Data | `0x19000` | `0x2000` | OTA 启动状态 |
| APP0 | `0x20000` | `0x300000` | 当前/候选应用 |
| APP1 | `0x320000` | `0x300000` | OTA 应用槽 |
| LittleFS | `0x620000` | `0x9D0000` | 台标及持久化文件 |
| Core Dump | `0xFF0000` | `0x10000` | 崩溃转储 |

网页 OTA 不支持上传 16 MiB 完整镜像。后台“固件升级”上传应用 `.bin`；4.4.0 及以后还可通过“资源镜像升级”上传构建目录的 `littlefs.bin`。完整无线升级时，先升级应用，重启后再升级资源。资源升级会保留 Wi-Fi、管理密码及其他 NVS 设置，但会替换 LittleFS 中的播放列表；4.4.4 会自动恢复内置电台，自定义电台应提前备份。整片擦除后仍必须同时写入启动程序、分区表、OTA 引导、应用和 LittleFS 镜像。

## 首次启动或恢复出厂后的配置

1. 设备启动后会创建 `Radio-XXXX` 热点，后四位来自设备标识；密码为 `radio-setup`。
2. 使用手机或电脑连接该热点。若配网页面未自动打开，访问 `http://192.168.4.1/admin`。
3. 在后台点击“扫描网络”，选择 **2.4 GHz** Wi-Fi，输入密码并保存。最多可保存 5 个网络；设备启动时会优先连接信号最强的可用已保存网络。ESP32-S3 不能连接纯 5 GHz 网络。
4. 设备重启并联网后，将手机或电脑切回同一局域网，访问 `http://network-radio.local/`；若 `.local` 不可用，从路由器查询设备 IP。
5. 初始后台没有管理密码。建议在可信局域网中设置 8–63 位密码，登录用户名固定为 `admin`。
6. 配置电台、默认音量与页面主题，并测试播放。配网热点联网后仍会保留，密码与后台管理密码相互独立。

恢复出厂设置会清除 Wi-Fi、电台列表、音量、主题和管理密码，然后重启；内置台标资源不会因此删除。

## 构建与上传

依赖 `arduino-cli`、ESP32 Arduino Core 3.3.11 和 `ESP32-audioI2S`：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "ESP32-audioI2S"

./tools/build-firmware.sh
./tools/build-firmware.sh --upload --port /dev/cu.usbserial-XXXX
```

输出位于 `build/esp32-network-radio/`，构建脚本会自动校验 16 MiB Flash 布局。正式发布包存放在 `release/`；已归档的 OTA 发布包为 `network-radio-v4.4.5-ota.zip`，4.5.1 的当前编译固件位于构建目录。

不要提交 Wi-Fi 密码、管理员密码、私有流地址或设备专属密钥。
