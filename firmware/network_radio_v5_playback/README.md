# 网络收音机 V5：网络音频播放

V5 在 V4 的配网和播放列表基础上，使用固定版本 `ESP32-audioI2S 4.0.0` 播放 HLS/MPEG-TS/AAC 流，并输出至 MAX98357A。

- 选择网页中的“设为默认”会立即切换并播放该电台；
- 开机后会自动播放保存的默认电台；
- 预置凤凰 HLS 流；
- `GET /api/player/status` 可读取播放状态；
- `POST /api/player/play`、`/stop`、`/volume?value=0..21` 可控制播放。

要求：路由器 Wi-Fi 已通过 V4 网页保存；`Radio-1FB8` 热点在开发阶段仍可用。MAX98357A 接线保持 GPIO4/5/6、5 V 与 OUT+/OUT-。
