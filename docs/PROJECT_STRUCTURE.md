# 项目维护说明

## 代码入口

`sources/esp32-network-radio/` 是唯一的当前开发入口。`esp32-network-radio.ino`
包含播放器、网络、网页管理、LittleFS 迁移和 WS2812B 状态灯；`partitions.csv`
为 16 MiB Flash 分区表，`data/logos/`
保存构建 LittleFS 镜像所需的台标。

根目录 `README.md` 是唯一的项目使用说明；不要在源码目录创建重复 README。
每次修改固件版本号时必须同步更新根目录 `CHANGELOG.md`。

## 版本维护

发布新版本时直接更新 `sources/`，不要为版本号创建新的源码目录。本地中间文件统一
写入 `build/`，不得复制回源码目录。

根目录使用 `./tools/build-firmware.sh` 编译和校验当前源码。

## 发布包

`release/` 只保存正式的版本化 ZIP 包，例如当前保留的上一版 `network-radio-v3.0.2-source.zip`。
解压目录、设备备份和临时打包目录不得留在这里。

## 资源写入注意事项

台标源文件位于 `sources/esp32-network-radio/data/logos/`，发布脚本生成对应的 LittleFS 镜像。
完整写入 LittleFS 会替换设备上的资源分区；烧录应用程序本身不会替换该分区。设备电台列表有
LittleFS/NVS 恢复逻辑，但执行资源全量刷新前仍应先备份设备配置。
