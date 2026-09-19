# Camera 模块

## 职责与入口

Camera 负责 V4L2 单平面视频采集，不负责 GStreamer 解码、AI 或编码。主要代码是 `src/camera/v4l2_video_source.*`、`src/camera/video_capture_service.*`，接口位于 `src/camera` 和 `include`，CMake target 为 `rkmon_camera`。

## 数据流

```text
V4L2 device -> mmap buffers -> private frame storage -> BoundedQueue<VideoFrame>
```

打开设备后查询能力、格式和 buffer，使用 poll 等待帧，在 dequeue 后复制到应用自有内存，再重新入队驱动 buffer。下游只能持有自有帧，不能保存已归还的 mmap 裸指针。

当前默认设备是 SYD USB Camera 的 `/dev/v4l/by-id/` 路径，默认 MJPEG 1920x1080@30。支持 MJPEG/YUYV 协商；实际结果记录在日志。队列容量有限，视频分支满时按 `drop_oldest` 丢弃待处理旧帧。

## 配置和退出

配置来自 `[camera]`：设备、宽高、格式、FPS、driver buffer、队列容量、poll 超时、连续超时阈值和重连间隔。设备错误或连续读取超时达到阈值会报告服务故障并关闭队列。停止会唤醒阻塞读取、停止生产并 join；当前自动重连策略尚未作为正式运行时恢复闭环验证。

帧包含 sequence、单调时间戳、接收时间、格式、尺寸和 stride 等元数据。接收时间是主控取帧时间，不等同于传感器曝光时间。

## 验证边界

`tests/camera_probe.cpp` 和相机服务测试覆盖节点错误、格式检查、慢消费者、关闭和重启。板端已验证 MJPEG/YUYV 短时采集及有界队列丢帧策略，但不代表驱动长期零丢帧，也不包含图像视觉质量、物理拔插和长稳验证。独立板端检查使用 `script/validate-video-board.sh`。
