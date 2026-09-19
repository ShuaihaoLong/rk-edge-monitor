# 第二阶段：硬件解码闭环验证

2026-09-14，在 RK3588 `elf@192.168.100.11` 完成。板端部署目录 `/tmp/rkmon-stage2.JwmbhL`，未安装或修改板端软件包。日志位于 [本轮日志目录](validation/video-stage2-2026-09-14/)。

## 实现范围

正式应用通过服务工厂连接：

```text
V4L2VideoSource → VideoCaptureService → 有界 JPEG 队列
  → VideoProcessService → IVideoDecoder（GstVideoPipeline）
  → appsrc → jpegparse → mppjpegdec format=NV12 → appsink
  → 有界 NV12 队列
```

采集服务不依赖 GStreamer。解码服务只接收视频帧队列 provider 和解码接口，provider 在 start 时读取上游当次队列。GStreamer 类型通过 Pimpl 保留在底层 `.cpp`。服务管理器先启动采集、再启动解码；退出时先通知所有服务停止，再逆序 join。

单次 decode 只有一个 JPEG 在途。压缩输入复制到 GstBuffer；解码输出按 GstVideoFrame 的平面地址和 stride 逐行复制成自有 NV12 内存，不借用硬件缓冲区。输出 stride=width、UV 偏移=width×height，宽高要求偶数，大小=width×height×3/2。来源 sequence/timestamp 保留，时间戳不是设备曝光时间。超时升级为故障，迟到的输出不会继续与下一请求匹配。

`request_stop` 使用原子标志，appsink 分段等待最多 20 ms 后重新检查；输入队列同样使用 20 ms 有限等待，不关闭上游拥有的队列。实际完整关闭还包含线程调度和 GStreamer/驱动回收时间，不能把 20 ms 当作端到端停止保证。

## 实测

| 检查 | 结果 |
| --- | --- |
| 主机 CTest | 10/10 通过 |
| ARM64 构建 | 成功，GStreamer/app/video 从板端 sysroot 链接 |
| 板端 video service 单元测试 | 通过，覆盖队列丢旧帧、空闲停止、解码中停止、故障上报、关闭上游拒绝启动、重启切换队列 |
| 硬解内容测试 | 合成 320×240 黑色 JPEG；校验完整 Y/UV 内容、NV12 长度、来源序号/时间戳及关闭后内存有效 |
| 后端重启、坏 JPEG 超时、停止等待 | 通过 |
| 相机 1080p MJPEG，30 秒 | 采集 888、解码 888、消费 888，队列丢帧 0，29.4823 FPS |
| 同上进程指标 | 平均采集到消费延迟 21.967 ms；CPU 57.7129%；峰值 RSS 41136 KiB |
| 消费者每帧等待 100 ms，10 秒 | 采集 297、解码 297、消费 100、输出丢帧 193，29.1578 FPS |
| 同上进程指标 | 平均延迟 135.928 ms；CPU 50.8108%；峰值 RSS 56184 KiB |
| 正式程序 SIGINT / SIGTERM | 均退出码 0，收到信号后约 0.214 / 0.215 秒退出；连续运行可重新打开相机 |
| 临时插件目录排除 MPP | 明确报告 mppjpegdec 缺失并以 1 退出，启动失败回滚采集；未修改系统插件 |

CPU 是进程用户态+内核态 CPU 时间/墙钟时间，相当于单个 CPU 核心百分比；峰值 RSS 包含采集、GStreamer、解码输出、验证工具持有的帧等，不能当作解码器单独占用。帧率包含启动和停止开销。消费延迟使用采集完成后的应用时间戳，不包含曝光到取帧的全部耗时。

## 验证边界

- 本轮确认实际硬解、图像布局和主要生命周期路径，没有做小时级稳定性、物理拔插或零拷贝验证。
- 厂商插件仍报告 `MPP is not able to generate pts` 和退出时 `no matched frame`；当前接口利用单请求在途保留来源元数据。未来批量异步解码必须重新设计帧关联，不能照搬顺序匹配。
- 正式应用尚无 AI/编码消费者，因此默认 NV12 输出队列会持续丢旧帧，属于有界队列的预期行为。
- RTSP、编码分支、DMA-BUF 共享均不在本阶段实现范围。
