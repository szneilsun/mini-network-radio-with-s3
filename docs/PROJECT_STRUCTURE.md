# 项目维护说明

## 代码入口

`firmware/network_radio_v3_2_status_lcd/` 是唯一的当前开发入口。其 `.ino`
包含播放器、网络、网页管理、LittleFS 迁移和 LCD 状态屏；`chinese_font_16.h`
为 LCD 中文点阵字库，`partitions.csv` 为 16 MiB Flash 分区表。

## 历史版本

`firmware/network_radio_v*` 中的目录均为历史快照。它们不参与默认构建，也不应被
重命名或覆盖；需要构建时通过 `ESP32_SKETCH_DIR` 显式选择，例如：

```bash
ESP32_SKETCH_DIR="$PWD/firmware/network_radio_v3_1_lcd" ./tools/build-firmware.sh
```

## 发布包

`release/network-radio-v3.2.1-status-lcd-source/` 是当前发布备份。它自带
`libraries/`、台标资源和 `build.sh`，适合复制或上传至 GitHub。发布包内的
`build/` 与 `binaries/` 是生成物，永远不作为源码提交。

## 资源写入注意事项

台标资源位于发布包的 `network_radio_v3_2_status_lcd/data/logos/`。完整写入
LittleFS 会替换设备上的资源分区；烧录应用程序本身不会替换该分区。设备电台列表有
LittleFS/NVS 恢复逻辑，但执行资源全量刷新前仍应先备份设备配置。
