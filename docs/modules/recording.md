# Recording 模块

## 进程边界

录像不是 CMake C++ target。MediaMTX 负责真正的 fMP4 分段和 Playback API；`src/recording/service.py` 是由 systemd 启动的独立 Python 服务，负责索引、事件、回放业务 API 和容量管理。

## 录制链路

```text
rkmon RTSP -> MediaMTX -> /userdata/rkmon-video/camera/YYYY-MM-DD/*.mp4
                         -> segment hook -> Unix datagram -> service.py
```

MediaMTX 配置 `record=true`、`recordFormat=fmp4`、约 1 分钟 segment，并调用 `notify.py` 的 `segment_open`/`segment_complete`。hook 只发送路径和事件类型，发送失败不应阻塞录制。

## 索引与恢复

`service.py` 使用 `/userdata/rkmon-video/index.sqlite3`，维护 `recordings` 和 `events` 表。完成通知会用 `ffprobe` 检查视频流、时长和文件是否仍在增长；通知丢失时每约 10 秒扫描目录，对稳定文件标记为 `recovered`。路径必须位于 `camera` 目录且扩展名为 `.mp4`，防止路径穿越和软链接逃逸。

录像状态包括 `writing`、`ready`、`recovered`、`invalid` 和 `deleting`。删除先写 `deleting`，再删文件和索引，进程中断后可以继续处理。

## 事件聚合

AI 通过 Unix Datagram 发送检测结果。服务按 session、generation 和 class 聚合，3 秒内同类目标延长同一事件，revision 去重，置信度保留较高值。事件写入 SQLite，供网页按时间定位录像；录像服务不可用时 AI 不应被同步阻塞。

## API 与回放

API 监听 `127.0.0.1:9010`：

- `/api/recordings?date=YYYY-MM-DD`：当天录像和事件。
- `/api/recordings/days`：有可播放录像的日期。
- `/api/recordings/status`：索引服务和磁盘状态。
- `/api/playback?start=...&duration=...`：返回选定分段的回放信息。
- `/api/recordings/<id>/download`：经 Nginx `X-Accel-Redirect` 下载。
- `/api/recordings/<id>/media`：经 Nginx 受保护地读取完整 MP4，使浏览器原生控件可 Range seek。

网页 `web/recordings.js` 使用时间片段定位和列表续播；录像按最新时间倒序展示。录像服务本身不搬运大文件，Nginx 通过 internal alias 发送文件。

## 清理策略

默认保留 7 天，预留至少 1 GiB；可通过 `max_gib` 设置总容量上限。正在写入或最近 90 秒仍变化的文件不会删除。清理录像时同步删除过期事件；空间仍不足则把服务状态标为 degraded 并记录错误。

## 验证边界

Python 单测覆盖索引幂等、恢复、路径安全、事件聚合和删除恢复；集成测试覆盖 MediaMTX 分段、Playback 解码、重启恢复、Nginx 下载和浏览器回放。长时间磁盘压力、断电一致性和多路录像尚未验证。
