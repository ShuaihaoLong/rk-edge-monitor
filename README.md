rk3588 ip: 192.168.100.11
user.name: elf
user.password: elf

## 构建与启动

项目统一使用 C++17，禁用编译器语言扩展。线程使用 `std::thread`、显式停止和 `join`；后续开发保持 C++17 兼容。

```bash
bash script/build.sh --target host --test
bash script/build.sh --target rk3588
bash script/start.sh
```

默认启动 `build/rk3588/src/app/rkmon`，读取 `config/rkmon.ini`。交叉编译依赖 ARM64 GCC/G++ 和 `.local/sysroots/rk3588`，可用 `RK3588_SYSROOT` 指定已有 sysroot。ARM64 程序必须在板端运行。

脚本可从任意目录调用，支持：

```bash
bash /path/to/rk-edge-monitor/script/start.sh --target host --config /tmp/host.ini
bash script/start.sh --binary /path/to/rkmon --config /path/to/rkmon.ini
```

主机没有相机时，在配置中同时设置 `[camera] enabled = false` 和 `[video] enabled = false`。脚本前台运行，使用 `exec` 保留 PID、信号和退出码；不会自动构建、后台运行或部署。相对命令行路径以调用目录为基准。

直接运行程序仅支持通用参数：

```bash
./rkmon --config /path/to/rkmon.ini
./rkmon --help
```

省略 `--config` 时，读取当前目录下的 `config/rkmon.ini`。原有 `--device/--width/--height/--format/--fps` 参数已移入 INI。Ctrl+C 或 SIGTERM 正常关闭，设备/配置错误返回非零状态。

## 统一 INI 配置

配置入口为 [config/rkmon.ini](config/rkmon.ini)，启动时一次性解析和校验，修改后重启生效。

| 配置节 | 当前字段 |
| --- | --- |
| `[app]` | `control_mailbox_capacity` |
| `[logging]` | `console`、`file`、`level`、`max_file_size`、`rotated_files` |
| `[video]` | `enabled`、`timeout_ms`、`queue_capacity` |
| `[camera]` | `enabled`、`device`、`width`、`height`、`format`、`fps`、`buffer_count`、`queue_capacity`、`poll_timeout_ms`、`max_consecutive_timeouts` |

节名和键名区分大小写，布尔值使用 `true/false`，注释以 `#` 或 `;` 开头且单独占一行，值不加引号。不支持行尾注释、变量展开或热加载。相对文件路径以 INI 文件所在目录为基准，避免切换工作目录后日志路径变化。

未知节/键、重复节/键、错误类型和越界值会拒绝启动。省略相机节或设置 `enabled=false` 时不装配相机服务；启用时必须提供 `device`。日志级别支持 `trace/debug/info/warn/error/critical/off`；`file` 为空可关闭文件输出，但必须保留控制台或文件中的至少一种。

相机参数范围：宽高 1–16384，FPS 1–1000，驱动 buffer 2–64，应用 queue 1–64，poll 超时 1–60000 ms，连续超时阈值 1–1000。这是配置校验范围，具体分辨率/帧率仍由硬件协商决定。

增加其他设备时，在 `RuntimeConfig` 和配置读取中增加对应节，在 `src/app/application.cpp` 的 `make_service_factory()` 中注册其 `IService` 实现；`main` 和通用生命周期逻辑无需添加设备参数处理。

## 当前 USB 相机能力

已实现 V4L2 单平面 mmap 采集、独立采集线程、只读自有帧数据、有界队列、故障退出和优雅关闭。仓库 INI 默认配置 SYD USB Camera，MJPEG 1920×1080@30，使用稳定的 `/dev/v4l/by-id/usb-SYD_USB_Camera_200901010001-video-index0` 路径。

实机 `/dev/video21` 为视频节点，`/dev/video22` 为元数据节点；编号可能变化。相机声明 MJPEG 1080p@30、YUYV 1080p@5 / 720p@10 / 480p@30。启动日志记录协商结果，退出日志记录采集 FPS、应用队列丢帧和超时数。

当前正式应用默认将 MJPEG 送入 GStreamer/MPP 硬件解码，并在解码输出队列保留最新 NV12 帧。尚未接入 AI 或编码消费者，因此解码输出队列丢帧增加属于预期。不会显示、保存或推送视频；连续读取超时达到配置阈值或设备错误，会关闭队列并让应用退出。自动重连尚未实现。

板端测试消费者与正式程序使用同一份 INI：

```bash
./build/rk3588/tests/rkmon_camera_probe --config config/rkmon.ini --seconds 10
./build/rk3588/tests/rkmon_camera_probe --config config/rkmon.ini --seconds 10 --delay-ms 100
./build/rk3588/tests/rkmon_camera_probe --config config/rkmon.ini --seconds 5 --yuyv
```

测试工具的 `--yuyv` 临时覆盖为 YUYV 640×480，`--device` 可临时覆盖节点；不修改 INI。工具只检查帧数据并输出统计，不保存图像。应用队列丢帧不包括驱动丢帧，FPS 包含启动等待。

## 代码入口与阶段总结

- `src/app/main.cpp`：解析配置路径、加载配置、信号线程和程序退出。
- `src/app/config.cpp`：INI 解析、配置校验、相对路径解析。
- `src/app/application.cpp`：服务工厂负责设备装配；Application 负责通用服务生命周期、控制邮箱和故障处理。
- `src/camera/video_source.hpp`：视频源接口、采集配置和读取结果。
- `src/camera/v4l2_video_source.hpp/.cpp`：V4L2 设备配置、MMAP 缓冲区和取帧。
- `src/camera/video_capture_service.hpp/.cpp`：采集线程、输出队列和服务生命周期。
- `tests/`：核心组件、配置、模块装配、启动脚本和相机测试。

已接通 `V4L2 → 采集队列 → appsrc → jpegparse → mppjpegdec format=NV12 → appsink → NV12 队列`，后续接 RGA / RKNN / MPP 编码。当前功能与框架检查见 [框架检查记录](docs/framework-review.md)，历史采集验证见 [相机验证记录](docs/usb-camera-validation.md)。

板端 GStreamer/MPP 能力检查使用 `script/validate-video-board.sh`，执行方式和验证边界见 [视频能力验证](docs/video-capability-validation.md)。

## 硬件解码闭环

`[video] enabled=true` 要求相机已启用且配置为 MJPG；省略该节默认不创建解码服务。`timeout_ms` 默认 2000（范围 1–60000），是单帧等待时限；`queue_capacity` 默认 4（范围 1–64），控制解码后的输出队列。仓库板端配置已启用解码。硬件插件缺失或解码失败会明确报告故障，不静默切换软件解码。

- `src/video/interfaces/video_decoder.hpp`：不含 GStreamer 类型的解码契约。
- `src/video/gstreamer/gst_video_pipeline.*`：Pimpl 隔离管线、GstBuffer 所有权、Bus 错误和 NV12 布局处理。
- `src/video/services/video_process_service.*`：仅依赖解码接口与队列，接入现有 IService 生命周期。
- `src/app/application.cpp`：唯一的采集/解码组装位置，先启动采集，再获取其当前队列并启动解码。

第一版每次一个 JPEG 请求在途。压缩输入复制到 GstBuffer，硬解输出通过 GstVideoFrame 的实际 stride/平面信息逐行复制为紧密排列 NV12：Y 偏移 0，UV 偏移 width×height，stride=width，大小 width×height×3/2。输出保留来源 sequence/timestamp；超时立即升级为故障，避免迟到帧与新请求错配。该实现优先保证生命周期清楚，不是零拷贝。

`RKMON_WITH_GSTREAMER` 在 aarch64 默认开启，主机默认关闭，主机服务测试不需要安装 GStreamer。开启时要求 sysroot 提供 GStreamer/app/video >= 1.20。显式切换可用 `cmake -S . -B build/host -DRKMON_WITH_GSTREAMER=OFF`；构建脚本会保留已有缓存选择。

板端有限时长验证（不保存图像）：

```bash
./build/rk3588/tests/rkmon_video_tests
./build/rk3588/tests/rkmon_gst_decoder_tests
./build/rk3588/tests/rkmon_video_probe config/rkmon.ini 30
./build/rk3588/tests/rkmon_video_probe config/rkmon.ini 10 100
```

最后一个参数是消费者每帧延迟毫秒数。probe 校验 NV12 内存布局、来源序号递增、关闭后帧可继续持有，并输出帧率、队列丢帧、平均采集到消费延迟、进程 CPU 和峰值 RSS。`gst_decoder_tests` 需板端 `jpegenc` 生成黑色测试图，并验证硬解后的 Y/UV 内容、重启、坏 JPEG 超时和停止等待。

2026-09-14 板端闭环验证：1080p MJPEG 连续 30 秒采集/解码/消费均为 888 帧（29.48 FPS），平均采集到消费延迟约 22 ms；慢消费者每帧等待 100 ms 时仍解码约 29 FPS，并按容量丢旧帧。硬解图像内容、重启、坏 JPEG 超时、停止等待和正式程序 SIGINT/SIGTERM 退出均通过。完整记录见 [解码闭环验证](docs/video-decode-validation.md)。这些短时结果不等同于长期稳定性或零拷贝保证。


### 第三阶段：硬件编码与网页监控

板端访问地址：`http://192.168.100.11:9000/`。前端代码在 `web/` 管理，原生 HTML/CSS/JS，无需 Node.js 构建。
页面仅显示实时画面与连接状态，使用 MediaMTX v1.21.0 自带的 WHEP reader，许可证保存在 `web/vendor/`。

链路：V4L2 MJPEG → `mppjpegdec` → NV12 有界队列 → `mpph264enc` → 本机 RTSP → MediaMTX → WebRTC → 浏览器。
默认 1920×1080、30 fps、H.264 Baseline、4 Mbps、GOP 30；无音频。当前 NV12 在模块边界复制，尚未实现 DMA-BUF 零拷贝。

- `src/video/interfaces/video_publisher.hpp`：发布接口和配置，不暴露 GStreamer 类型。
- `src/video/gstreamer/gst_rtsp_publisher.*`：GStreamer 管线、NV12 布局转换、时间戳、硬件编码及 RTSP 错误检测。
- `src/video/services/video_stream_service.*`：消费解码队列、线程生命周期及故障上报，依赖注入 `IVideoPublisher`。
- `src/app/application.cpp`：按采集、解码、发布顺序组装，停止时逆序请求退出和回收。
- `config/rkmon.ini` 的 `[stream]`：启用开关、发布 URL、码率、关键帧间隔和无编码输出超时；帧率沿用相机配置。

只在编码前丢旧帧；编码后的 H.264 直接进入 RTSP，不任意丢弃参考帧。`encoded_frames` 是编码输出计数，不代表浏览器已接收。
服务层通过接口隔离具体媒体实现；Nginx 和 MediaMTX 是独立进程，应用不承担 HTTP 或 WebRTC 信令。

| 服务 | 配置 | 端口 |
| --- | --- | --- |
| Nginx | `config/nginx-monitor.conf` | TCP 9000，静态页面与 WHEP 同源代理 |
| MediaMTX | `config/mediamtx.yml` | 本机 TCP 8554/8889，媒体 UDP 8189 |
| rkmon | `config/rkmon.ini` | 向本机 `rtsp://127.0.0.1:8554/camera` 发布 |

当前配置用于可信局域网，无登录功能；浏览器所在网络需能访问板卡 TCP 9000 和 UDP 8189。
更换板卡 IP 时更新 `mediamtx.yml` 的 `webrtcAdditionalHosts` 和 `webrtcAllowOrigins`。
本阶段未配置跨公网访问所需的 HTTPS、认证或 TURN。

离线部署（Nginx 和 Rockchip GStreamer 插件需预先安装）：

```bash
bash script/build.sh --target rk3588 --jobs 4
# 安装包放在 .local/downloads/，脚本会校验固定版本的官方 SHA256。
bash script/package-monitor.sh
scp build/deploy/rkmon-deploy.tar.gz elf@192.168.100.11:/tmp/
ssh -t elf@192.168.100.11
# 以下在板卡执行；sudo 在终端中输入密码。
tar -xzf /tmp/rkmon-deploy.tar.gz -C /tmp
sudo bash /tmp/rkmon-deploy/script/install-monitor.sh
```

安装位置 `/opt/rkmon`，运行用户 `elf`，服务配置在 `config/systemd/`。
安装脚本覆盖本项目配置、备份旧文件到 `/opt/rkmon-backup.*`，启用三个服务开机启动，不修改其他 Nginx 站点。
部署固定版本：MediaMTX 1.21.0 linux_arm64；Nginx Ubuntu 包 1.18.0-6ubuntu14.20。

板端维护命令：

```bash
systemctl status rkmon mediamtx nginx
journalctl -u rkmon -u mediamtx -f
sudo systemctl restart rkmon
# 维护时同时停止，避免 rkmon 自动重启时通过 Wants 再次启动 MediaMTX。
sudo systemctl stop rkmon mediamtx
sudo systemctl start mediamtx rkmon
```

媒体连接故障上报到应用后退出，由 systemd 间隔 2 秒重启；网页 reader 自动重连，15 秒无播放进展会重建会话。
2026-09-14 验证：主机 11 项 CTest 通过、ARM64 交叉编译通过；板端 Chromium 实际播放 1920×1080，
30 秒采样增加 890 帧（约 29.7 fps）；停止 MediaMTX 后应用自动重启，原网页约 18 秒后重新建立媒体会话，无需刷新。
已验证板端浏览器经板卡 IP 访问；电脑/手机浏览器的实际网络播放仍需在对应终端确认。

补充复测：相机采集端后续降到 16.26 fps，采集 2044 帧、解码 2044 帧、提交编码 2044 帧，
采集及解码队列丢帧均为 0；浏览器约 16.3 fps。未将配置 30 fps 视为固定实测帧率。
相机当前 `exposure_auto=3`、`exposure_auto_priority=1`，允许自动曝光改变帧率，
是输入波动的可能原因；本次未修改相机曝光策略。
控制项定义见 [Linux V4L2 文档](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-camera.html)。


### PC 一键部署与更新

在 PC/WSL 执行，首次部署和后续更新使用同一入口：

```bash
bash script/deploy.sh
# 先检查计划，不构建、不连接板卡、不修改文件：
bash script/deploy.sh --dry-run
# 已完成 ARM64 构建时，复用产物：
bash script/deploy.sh --skip-build
# SSH 别名/目标、并行数和安装包可以指定：
bash script/deploy.sh --host elf@192.168.100.11 --jobs 4
```

支持从任意工作目录调用。默认检查 SSH 后构建，再打包、上传独立临时目录、调用板端安装脚本、检查服务及 HTTP，最后清理临时文件。
SSH 使用密钥登录；板端 sudo 如需密码，会在终端询问。构建、传输、安装或健康检查失败返回非零，不显示部署成功。
安装期间短暂停流；旧文件备份到 `/opt/rkmon-backup.*`。更新会覆盖板端项目配置，部署前将需要保留的修改同步到仓库 `config/`。
`--host` 仅改变 SSH 连接目标；运行用户仍为 `elf`，访问地址和 WebRTC 地址需按配置调整。
本脚本不安装系统依赖，板端需预先具备 Nginx、Rockchip MPP/GStreamer 和 curl。
后续部署变动统一维护此入口及其打包/安装子脚本。

`src/video` 按职责组织：

```text
src/video/
├── interfaces/   # 解码与发布契约、参数，不包含 GStreamer 类型
├── services/     # 消费帧队列、线程生命周期和故障上报
├── gstreamer/    # MPP 解码、编码及 RTSP 管线实现
├── factory/      # 根据构建选项选择具体实现
└── CMakeLists.txt
```

服务只依赖接口；工厂连接接口与具体后端，应用层负责组装。目录调整不改变对外运行行为。
