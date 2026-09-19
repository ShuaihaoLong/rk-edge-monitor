# 板端视频能力验证

> 第一阶段板端能力验证记录（2026-09-10）。它验证独立 GStreamer/MPP 管线，不代表当时的正式服务状态；当前视频实现见 [Media](modules/media.md)。

## 当前状态

2026-09-10，已通过 SSH 在 `elf@192.168.100.11` 完成第一阶段验证。无需安装或升级板端组件，现有 GStreamer + Rockchip MPP 可用于下一阶段的 MJPEG 解码模块。

板端目录：`/tmp/rkmon-video-stage1.FlXysc`。最终结果见 [summary.tsv](validation/video-stage1-2026-09-10/verified/summary.tsv)，原始日志保存在同目录；`initial/` 为只检查退出码的初次诊断，不作为最终验收依据。

| 项目 | 实测结果 |
| --- | --- |
| 系统 | aarch64，Linux 5.10.209，Ubuntu 22.04.5 |
| GStreamer | 1.20.3 |
| Rockchip 插件 | `rockchipmpp` 1.14.4，包含 `mppjpegdec`、`mpph264enc` 等 |
| MPP / RGA | MPP 包 1.5.0 + 20240717，RGA 包 2.2.0 + 20231208；插件链接 `librockchip_mpp.so.1`、`librga.so.2` |
| C++ appsink 示例 | ARM64 产物运行成功，收到 1 帧，ldd 无缺失依赖 |
| GStreamer 基础管线 | 末端收到 1 个 buffer |
| 1080p NV12 → MPP H.264 → h264parse | 末端收到 150 个 buffer，输出 byte-stream / AU，解析后 profile=high |
| H.264 编码 → OpenH264 软件解码 | 末端收到 30 帧 I420；板上没有 avdec_h264，使用已有 openh264dec |
| 相机 1080p MJPEG → 软件 JPEG 解码 | 末端收到 150 帧 |
| 相机 1080p MJPEG → MPP JPEG 解码 | 末端收到 150 帧，默认输出 NV16 |
| MPP JPEG 显式转换 NV12 | `mppjpegdec format=NV12`，末端收到 150 帧 NV12 |
| RTSP 依赖 | 运行库 `libgstrtspserver-1.0-0` 1.20.1-1 已安装；开发包 `libgstrtspserver-1.0-dev` 缺失，pkg-config 查找失败 |

## 对下一阶段的约束

1. **必须显式指定解码输出格式。** 实测 `mppjpegdec ! video/x-raw,format=NV12` 只产生格式拒绝警告，进程仍以 0 退出，末端没有图像。使用 `mppjpegdec format=NV12 ! video/x-raw,format=NV12` 后才收到 NV12 图像。验证脚本已增加末端 buffer 检查，防止这种假成功。
2. **正式模块使用 GstVideoInfo/GstVideoMeta 获取布局。** 默认 NV16 帧大小为 4,177,920 字节，大于紧密排列的 1920×1080×2，不能按宽高推算平面地址。当前工程的 PixelFormat 未定义 NV16，下一阶段可先固定 NV12 输出。
3. **不能将本轮结果解释为稳定 30 FPS 或零丢帧。** 硬解两条管线各输入并输出 150 帧，总运行约 5.57 秒，包含启动等待。日志仍有驱动 lost-frames、copy-threshold、MPP PTS 和退出阶段 no-matched-frame 警告；输出齐全不能证明相机从未丢帧。需在下一阶段测时间戳、持续帧率及延迟。
4. **厂商插件可运行不代表零拷贝。** 插件依赖与日志确认使用 MPP/RGA 路线，但本轮未跟踪 DMA-BUF 传递、CPU 使用率或逐段内存拷贝。
5. **RTSP 开发包留待 RTSP 阶段补齐。** 随后同步到新的 sysroot 快照；当前缺失不阻碍使用 appsrc/appsink 实现解码闭环。本轮没有安装依赖。
6. H.264 软件回解初始出现 `profile=High` caps 拒绝警告，解析到 `profile=high` 后输出 30 帧；后续编码接入应保留 `h264parse` 并检查协商与首帧行为。

下一阶段建议的边界为：现有 V4L2 采集队列 → appsrc → jpegparse → mppjpegdec format=NV12 → appsink。**本轮使用 v4l2src 独立验证硬件能力，尚未把现有采集服务接入 appsrc，也未进行图像视觉质量检查。**

## 执行方式

脚本可单独复制到板端，无需在板端安装或构建工程：

```bash
scp script/validate-video-board.sh elf@192.168.100.11:/tmp/rkmon-validate-video-board.sh
ssh elf@192.168.100.11 'bash /tmp/rkmon-validate-video-board.sh'
```

默认使用工程中的 SYD 相机稳定路径，1920×1080 MJPEG、30 FPS，每条主要管线输入 150 帧，最长运行 30 秒。相机必须空闲；脚本不会结束已有采集服务。可以使用 `--device /dev/videoX` 指定节点，使用 `--frames` 和 `--timeout` 调整规模。所有帧流向 `fakesink`，不保存图像或视频。

日志默认写入板端 `/tmp/rkmon-video-check.*`：

- `inventory.log`：系统、用户权限、设备节点、包版本和相机格式。
- `inspect-*.log`：插件来源、属性、输入和输出能力。
- 各管线日志：准确命令、协商格式、末端 buffer 记录、错误和 EOS。
- `summary.tsv`：PASS / FAIL / SKIP；任何失败或跳过使脚本返回 1。

现有 C++ `appsink` 验证程序另行执行，用于确认交叉构建产物能在板端加载和取帧：

```bash
bash script/smoke-build.sh --jobs 4
scp build/rk3588-gst-smoke-make/gst-smoke elf@192.168.100.11:/tmp/rkmon-gst-smoke
ssh elf@192.168.100.11 'ldd /tmp/rkmon-gst-smoke && timeout -k 3s 15s /tmp/rkmon-gst-smoke'
```

## 检查范围

| 检查 | 目的 |
| --- | --- |
| `appsrc`、`appsink`、`mppjpegdec`、`mpph264enc` | 确认插件可加载，保存板端实际属性 |
| 合成图像 → `fakesink` | GStreamer 基础管线 |
| 合成 NV12 → MPP H.264 编码 → 解析 | 将编码能力与相机、JPEG 解码分开验证 |
| H.264 编码 → 软件解码 | 使用 `avdec_h264` 或 `openh264dec` 检查码流能否解码 |
| 相机 MJPEG → 软件 JPEG 解码 | 建立相机和软件解码基线 |
| 相机 MJPEG → MPP JPEG 解码 | 验证真实相机格式的硬件插件链路 |
| MPP JPEG 解码 → NV12 | 单独确认后续处理计划需要的格式协商 |

默认厂商插件名为 `mppjpegdec`、`mpph264enc`。缺少这些插件不等于硬件不支持，需结合板端插件清单进一步判断是否使用了其他实现或缺少厂商组件。

管线 PASS 表示命令正常结束且末端至少收到一个 buffer，详细数量写入 summary.tsv；这是诊断日志计数，不替代正式模块中的 appsink 计数和内容校验。后续阶段仍需补充图像内容检查、CPU/内存和长时间运行指标。此阶段不安装依赖，不更换驱动，也不实现正式视频处理服务。
