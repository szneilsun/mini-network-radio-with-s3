# 网络收音机 V3：首次配网与热点回退

这是独立于 V2 的 Arduino 草图。它仍输出 GPIO4/5/6 的 MAX98357A 测试音，并新增：

- 第一次启动时建立 `Radio-1FB8` 热点；
- 在 `http://192.168.4.1` 扫描、保存路由器 Wi-Fi；
- 保存后自动重启并连接路由器；
- 已保存网络连接超时或运行中丢失超过 20 秒时，自动重新开启热点；
- 成功连入路由器后可通过串口显示的 IP 或 `http://network-radio.local` 访问页面；
- Wi-Fi 凭据保存在 NVS，网页可清除它们。

开发阶段热点密码为 `radio-setup`。量产前应改为每台设备唯一的密码，并评估 NVS 加密。

硬件输出固定为 MAX98357A：GPIO4 → BCLK、GPIO5 → LRCLK、GPIO6 → DIN、稳定 5 V → VDD、GND 共地，扬声器只接 OUT+ 与 OUT-。`SD_MODE`、`GAIN` 可保持模块默认悬空。PCM5102A 的线路输出与配置脚不适用于此工程。

Arduino 配置与 V2 相同：`ESP32S3 Dev Module`、QIO 80 MHz、16 MB Flash、OPI PSRAM、`16M Flash (3MB APP/9.9MB FATFS)`、UART0 460800。
