# Media 模块

## 职责与入口

Media 负责帧封装、GStreamer/MPP 解码、OSD、H.264 编码和 RTSP 发布。公共接口在 `include/media/`，实现位于 `src/media/`；CMake targets 为 `rkmon_frame` 和 `rkmon_video`。

`RKMON_WITH_GSTREAMER` 控制 GStreamer 后端，`RKMON_WITH_RGA` 控制 RGA。aarch64 默认开启，主机测试默认关闭硬件后端。GStreamer 类型通过实现层隔离，服务层依赖解码/发布接口而不是直接操作管线。

## 解码链路

```text
VideoCaptureService
 -> JPEG bounded queue
 -> appsrc -> jpegparse -> mppjpegdec format=NV12
 -> appsink -> NV12 bounded queue
```

单次只允许一个 JPEG 请求在途。输入复制到 GstBuffer；当前闭环把输出复制为自有 NV12 帧，并保留来源 sequence/timestamp。必须通过 `GstVideoInfo/GstVideoMeta` 读取 stride、平面偏移和实际高度，不能从宽高推算 DMA 布局。

解码超时、坏 JPEG、GStreamer bus 错误会报告故障；停止期间 appsink 和输入等待使用有限超时，避免线程永久阻塞。输出队列是有界的，慢消费者会丢旧帧。

## 编码、OSD 和 RTSP

VideoStreamService 消费解码后的 NV12，使用 GStreamer/MPP 编码为 H.264，按 `[stream]` 配置码率、GOP 和超时，发布到 MediaMTX 配置的 RTSP URL。OSD 在编码前写入编码专用缓冲区，显示帧接收日期、时区和 UTC 偏移；AI 输入不应被 OSD 修改。

## 验证边界

板端已验证 MPP JPEG 解码到 NV12、H.264 编码、RTSP 相关插件和正式解码服务的关闭/重启/坏帧路径。当前结果不等于零拷贝、长期稳定或端到端客户端延迟保证。独立板端检查使用 `script/validate-video-board.sh`，正式服务测试位于 `tests/`。
