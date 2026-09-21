# 配置参考

主程序配置是 [config/rkmon.ini](../config/rkmon.ini)，启动时读取，修改后需要重启 `rkmon`。节名和键名区分大小写；注释必须单独成行；相对路径以 INI 所在目录为基准；未知、重复、类型错误或越界字段会拒绝启动。

## `[app]`

- `control_mailbox_capacity`：控制邮箱容量，必须为正数。

## `[logging]`

- `console`：是否输出控制台。
- `file`：日志文件路径，可为空，但控制台和文件不能同时关闭。
- `level`：`trace/debug/info/warn/error/critical/off`。
- `max_file_size`：轮转文件大小。
- `rotated_files`：保留的轮转文件数量。

## `[camera]`

- `enabled`、`device`、`width`、`height`、`format`、`fps`。
- `buffer_count`、`queue_capacity`、`poll_timeout_ms`、`max_consecutive_timeouts`、`reconnect_interval_ms`。

支持 V4L2 单平面 MJPEG/YUYV。设备节点建议使用 `/dev/v4l/by-id/` 稳定路径。宽高范围为 1--16384，FPS 为 1--1000，驱动 buffer 为 2--64，队列为 1--64。

## `[video]`

- `enabled`：是否启用 JPEG 到 NV12 的硬件解码服务。
- `timeout_ms`：单帧解码等待上限。
- `queue_capacity`：解码输出有界队列容量。

## `[stream]`

- `enabled`、`url`：RTSP 发布开关和地址。
- `bitrate`、`gop`、`timeout_ms`：编码参数和无输出超时。
- `osd_enabled`、`osd_timezone`：编码前时间 OSD。

## `[ai]`

- `enabled`、`model`、`labels`：模型、标签和开关。
- `result`：检测快照路径。
- `event_socket`：向录像索引服务发送检测事件的 Unix Socket。
- `fps`、`preprocess`、`input_memory`、`workers`、`core_policy`：推理频率、预处理、内存类型、工作线程和 NPU 核策略。

## `[mqtt]`

- `enabled`、`broker_host`、`broker_port`、`device_id`、`role`。
- `topic_prefix`、`publish_interval_ms`、`keepalive_seconds`。

## `[stm32]`

- `enabled`、`device`、`baud_rate`。
- `frame_timeout_ms`、`stale_timeout_ms`、`reconnect_interval_ms`。

启用 STM32 时要求 MQTT 同时启用。协议和主题见 [STM32 模块](modules/stm32.md)。

## 录像配置

录像索引服务使用 [config/recording.ini](../config/recording.ini)：`root` 为录像目录，`ads_root` 为独立广告目录，`event_socket` 为通知 Socket，`port` 为本机 API 端口，`timezone` 为显示时区，`retain_days` 为保留天数，`reserve_mib` 为磁盘保留空间下限，`reserve_percent` 为分区总容量的最低空闲百分比（整数 0～99，默认 40），两种下限取较大值。清理另留 1% 总容量余量。`max_gib=0` 表示不启用录像总容量上限。

## 本地显示配置

`config/display.json` 独立配置屏幕时区、中文字体、天气缓存路径/更新间隔和 `ads_root` 广告目录。
本地程序复用 `rkmon.ini` 的 RTSP 和 STM32/MQTT 配置，不另开摄像头或串口。
详见 [Display](modules/display.md)。
