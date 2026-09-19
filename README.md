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

- `include/media/decoder.hpp`：不含 GStreamer 类型的解码契约。
- `src/media/gstreamer/gst_video_pipeline.*`：Pimpl 隔离管线、GstBuffer 所有权、Bus 错误和 NV12 布局处理。
- `src/media/services/video_process_service.*`：仅依赖解码接口与队列，接入现有 IService 生命周期。
- `src/app/application.cpp`：唯一的采集/解码组装位置，先启动采集，再获取其当前队列并启动解码。

每次一个 JPEG 请求在途。压缩输入仍复制到 GstBuffer，硬解输出保留原始 DMA-BUF 及其 GstBuffer 引用，按 VideoMeta 记录实际 stride、UV 偏移和高度对齐。最后一个消费者释放后才归还解码池，不再逐行复制完整 NV12。输出保留来源 sequence/timestamp；超时立即升级为故障，避免迟到帧与新请求错配。

视频帧在 V4L2 `DQBUF` 成功后、图像复制前记录主控收帧时间：`timestamp` 使用单调时钟，
用于耗时和视频 PTS；`received_at` 使用系统时钟，供日期水印使用。解码沿用原帧的两种时间，
不会用解码完成时间覆盖；主控收帧时间不等同于相机曝光时间。

编码前在左上角叠加该帧的收帧日期时间。
`[stream] osd_enabled=true` 默认启用；`osd_timezone=Asia/Shanghai` 显示北京时间并标注 `UTC+0800`，
也可选 `UTC` 或 `local`（板端系统时区）。不会修改板端时区或系统时间。
文字直接绘制在编码器原有复制缓冲区的 NV12 小区域内，无 OpenCV、无额外整帧复制或 RGB 转换，
AI 分支共享的原始帧不受影响。水印进入编码视频，积压或冻结时显示的仍是对应帧的收帧时间。

画面左下角的“播放帧率”由浏览器统计，每约一秒更新，使用
[`requestVideoFrameCallback`](https://wicg.github.io/video-rvfc/) 的 `presentedFrames` 和
`presentationTime` 增量计算实际提交给合成器的视频帧率，不使用采集或编码配置的 FPS。
启动、缓冲、暂停、断线、页面隐藏及超过 1.5 秒没有呈现新帧时显示“—”；恢复后重新统计。
不支持该 API 的浏览器显示“—”。帧率属于当前浏览器的播放状态，只叠加在网页上，不写入编码视频。

2026-09-19 临时停止 `rkmon` 后对当前 SYD USB 相机采样 12 帧，V4L2 报告
`ts-monotonic, ts-src-soe`，即单调时钟、驱动标注曝光开始来源，短采样约 30 FPS。
这确认驱动提供时间戳及来源标志，不是对传感器曝光时间精度的独立校准；当前水印仍采用主控收帧时间。

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
默认 1920×1080、30 fps、H.264 Baseline、4 Mbps、GOP 30；无音频。解码 NV12 通过 DMA-BUF 共享给 AI 和编码分支；启用 OSD 时由 RGA 复制到编码专用 DMA 缓冲区，CPU 仅绘制文字，避免修改 AI 输入。

- `include/media/publisher.hpp`：发布接口和配置，不暴露 GStreamer 类型。
- `src/media/gstreamer/gst_rtsp_publisher.*`：GStreamer 管线、NV12 布局转换、时间戳、硬件编码及 RTSP 错误检测。
- `src/media/services/video_stream_service.*`：消费解码队列、线程生命周期及故障上报，依赖注入 `IVideoPublisher`。
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

`src/media` 按职责组织：

```text
include/media/
├── frame.hpp     # 跨 camera、media、ai 共享的视频帧类型
├── decoder.hpp   # 解码契约与参数
└── publisher.hpp # 发布契约与参数

src/media/
├── services/     # 消费帧队列、线程生命周期和故障上报
├── backends/     # MPP 解码、编码及 RTSP 管线实现
├── factory.cpp   # 根据构建选项选择具体实现
└── CMakeLists.txt
```

服务只依赖接口；工厂连接接口与具体后端，应用层负责组装。目录调整不改变对外运行行为。


### 第四阶段：NPU 能力验证

已用公开预转换的 RK3588 YOLOv8n INT8 模型完成 C API 单图验证；模型无需在 PC 重新转换即可运行。
板端 Runtime 2.1.0、驱动 0.9.6，35 次检测检查通过。独立探针位于 `examples/rknn-smoke/`，
该探针用于独立单图验证；实时视频接入见下节。依赖准备使用 `python3 script/prepare-ai-probe.py`。
模型来源、固定版本、复现命令和性能边界见 [AI 能力验证说明](examples/rknn-smoke/README.md)。


### 实时识别与网页显示

代码遵循 `b1b3e50` 的目录结构：公共数据与检测接口在 `include/ai`、`include/media`，
媒体管线在 `src/media`，检测器、预处理、推理服务和结果快照实现在 `src/ai`。

解码后的只读 NV12 帧分别进入编码队列和 AI 队列；AI 队列容量为 1，忙时保留最新帧。
`[ai]` 控制启用、模型/标签/快照路径和推理频率，默认 10 fps。AI 异常独立重试，不触发整个监控程序退出。
AI 预处理默认使用 RGA：NV12 转 RGB、缩放到模型有效区域、保持比例并补灰边（114），
沿用 BT.601 limited range。`[ai] preprocess=rga|cpu` 可显式选择后端；RGA 不可用或参数不支持时
报告 AI 故障并按既有策略重试，不静默回退 CPU。输入颜色范围或相机型号变化后需重新验证。

每个检测器持有固定的 640×640 RGB 输入缓冲区，RGA 输出句柄只导入一次；解码输入通过 fd
导入，支持硬件行/高度对齐。仅首次使用或输入尺寸变化时通过 RGA 清灰边，
随后复用同一输出区域。CPU 对照路径也复用 RGB 存储。两种后端共享缩放比例和补边坐标计算，
但硬件采样和颜色舍入不保证与 CPU 逐像素一致。

`RKMON_WITH_RGA` 在 aarch64 默认开启、主机默认关闭；交叉编译需要 sysroot 中的 RGA 头文件和
`librga`，板端也需匹配的运行库和 RGA 设备访问权限。沿用 `deploy.sh` 部署入口，其安装阶段的
`ldd` 检查会检查新增动态库依赖；部署包不自带或升级板端 RGA 驱动/运行库。
若构建时关闭 RGA，启用 AI 时须配置 `preprocess=cpu`。

NPU 并发由 `[ai] workers=1..3` 和 `core_policy=auto|split` 配置。仓库配置使用
`workers=3`、`core_policy=split`，分别绑定 NPU 核心 0/1/2；`auto` 交给 RKNN 运行时选择。
旧配置省略这两个字段时使用单线程、自动选核。每个工作线程独占模型 context、RGA 句柄和 RGB
缓冲区，独立初始化及失败重试；模型分别加载，内存占用会随工作线程数增加。

通用固定线程池位于 `include/core/thread_pool.hpp`，支持按线程索引提交任务，忙时拒绝提交。
推理服务每个线程最多一个任务，调度端只保留一帧最新输入；`fps` 是所有线程合计的提交上限，
当前仍为 10，若要提高吞吐可调整到 30（允许 1..60）。多线程不保证线性提速，需要实机比较。
结果只由调度线程写出，丢弃旧序号、旧相机代次及超过 700 ms 的结果，避免乱序覆盖。
停止时取消未开始任务，等待正在执行的 RKNN 调用返回，再由所属线程释放资源。

`[ai] input_memory=dmabuf` 为默认值：使用 `rknn_create_mem2` 分配非缓存输入，通过
`rknn_set_io_mem` 绑定，RGA 直接写入同一 fd，待同步完成后执行 NPU，不再调用 `rknn_inputs_set`。
`input_memory=copy` 保留原 RGA RGB 普通内存提交路径，供对照；`preprocess=cpu` 始终使用拷贝提交。
解码 DMA-BUF 为只读共享，编码关闭 OSD 时直接包装其 fd；开启 OSD 时使用最多 8 个独占 DMA32
缓冲区，RGA 同步复制后进行 CPU 文字绘制，并成对执行 `DMA_BUF_IOCTL_SYNC`。编码器释放之前
缓冲区不被复用，池满时丢弃本次未编码帧，避免无界增长。

这条链路消除了应用层完整 NV12 的 CPU 复制及 RGA→NPU 输入提交复制；V4L2 压缩 JPEG 采集、
JPEG appsrc 输入、NPU 输出获取仍保留现有路径，开启 OSD 仍有一次硬件复制，不声称全程零复制。
构建额外需要 `gstreamer-allocators-1.0`，板端用户需能访问 `/dev/dma_heap/system-uncached-dma32`。
解码器会检查实际 DMA 内存和布局，不以普通内存静默替代。共享帧的所有权及 CPU 映射封装位于
`include/media/dma_buffer.hpp`，布局检查位于 `include/media/nv12.hpp`。
`detections.json` 中 `worker_index` 表示工作线程索引，`source_generation` 表示相机代次；
`source_dma` 和 `input_dma` 分别表示当前帧是否使用解码 DMA 及 RKNN 共享输入。
`detections.json` 新增耗时字段，便于在相同视频和推理频率下对照：

| 字段 | 统计范围 |
| --- | --- |
| `preprocess_ms` | 预处理，包括 RGA 输入导入、参数检查和同步等待 |
| `input_ms` | 输入提交；DMA 路径初始化时已绑定，每帧该值接近零 |
| `npu_ms` | `rknn_run` 阻塞调用，不等于纯硬件执行时间 |
| `postprocess_ms` | 输出获取、后处理和结果组装 |
| `inference_ms` | 上述阶段总耗时，保持原字段含义 |

板端 `rkmon_rga_tests` 已验证黑白 NV12 转 RGB、灰边填充、输入尺寸变化及输出缓冲区复用；
传入模型和标签路径时，还会用合成帧验证三个独立 NPU 工作线程均能产出结果：

```bash
./rkmon_rga_tests /opt/rkmon/models/yolov8n.rknn /opt/rkmon/models/coco_80_labels_list.txt
```

该测试需真实 RGA/NPU，不自动加入主机 CTest。还包含带高度填充的 DMA 输入、RGB DMA 输出及
编码用 NV12 DMA 复制验证。`rkmon_gst_decoder_tests` 检查解码 DMA 的内容、关闭后保活和停止行为。
`rkmon_dma_pipeline_tests JPEG_1920x1080 MODEL LABELS RTSP_URL` 使用一张真实 JPEG 对照
DMA/拷贝输入的识别结果，并检查开关 OSD 的编码、共享输入不被修改。测试应使用独立 RTSP 服务，
不要覆盖在线发布路径。有限时长验证不替代长期稳定性和完整识别准确率测试。
暂不宣称具体加速比例。

结果以原图坐标输出到 `/run/rkmon/detections.json`，原子替换文件，Nginx 通过只读 `/api/detections` 提供访问。
运行目录由 systemd 创建，避免频繁快照写入持久存储；手动启动时需要可写的结果目录。
网页每 250 ms 获取最新快照，左侧最多保留 80 条识别日志；刷新页面重新开始记录，不作为持久审计日志。
视频右上角“显示识别框”使用当前浏览器的本地开关，不停止 NPU 推理、不影响其他浏览器。

识别框由 Canvas 绘制，按视频留黑边和原始尺寸映射坐标，无需 OpenCV，也不将框烧入 H.264。
关闭框后日志仍更新；结果过期、数据连接断开或视频冻结后清框。采用最新检测结果叠加，不保证与 WebRTC 逐帧同步，快速运动时可能有框滞后。

模型和标签已加入现有 `deploy.sh → package-monitor.sh → install-monitor.sh` 部署链路。
2026-09-14 完成验证：最新目录层级下 13 项主机 CTest 和 ARM64 构建通过；板端 1080p 视频产生实时结果，
实测快照中预处理到后处理约 26–60 ms（抽样，非完整性能基准）。浏览器验证了真实日志更新、检测框开/关、
刷新后开关状态保留，以及模拟检测 API 断线时清框且视频继续播放。测试框由浏览器临时注入，不改变板端推理数据。

### MQTT 设备状态

中心节点在板端运行 Mosquitto。局域网设备使用 MQTT `1883` 端口发布状态；网页通过 Nginx 的同源
`/mqtt` WebSocket 入口订阅，Mosquitto 的 `9001` 端口只监听回环地址。当前配置适用于可信局域网，
若跨网段或接入公网，应增加账号、ACL 与 TLS。

每台设备配置唯一的 `[mqtt].device_id` 和 `role=center|edge`，并向
`rkmon/devices/<device_id>/status` 发布 retained QoS 0 JSON。网页订阅
`rkmon/devices/+/status`，按设备动态生成状态卡。消息字段如下：

```json
{
  "schema": 1,
  "device_id": "rk3588-center",
  "role": "center",
  "ip": "192.168.100.11",
  "online": true,
  "camera_online": true,
  "heartbeat_interval_ms": 2000,
  "cpu_usage_percent": 12.3,
  "cpu_temperature_c": 48.6,
  "updated_at_ms": 1789387200000
}
```

发布端连接时注册 retained 离线遗嘱，正常退出也主动发布离线状态。网页同时以至少 8 秒、或三倍心跳周期未更新判定兜底，
避免异常网络下长期展示旧的在线消息。摄像头服务在设备不存在、采集错误或连续超时时保持进程运行，
将 `camera_online` 置为 `false` 并按 `reconnect_interval_ms` 后台重试；下游队列在重连期间保持有效，
摄像头恢复后视频、推流和识别链路可继续工作。

### STM32 温湿度与串口控制

已添加 `[stm32]` 模块，通过 `/dev/ttyS9`（115200/8N1）解析 STM32 的四字节 DHT11 上报，
使用简化 V1 帧和 CRC32 校验。温湿度发布到 `rkmon/devices/<device_id>/stm32/telemetry`，
状态发布到 `/stm32/status`；超过 5 秒没有有效采样标记离线，串口和 MQTT 独立重连。

`/stm32/command` 接收最多 128 字节原始二进制载荷，封装为 `TYPE=0x20` 下发；
`/stm32/command_result` 返回串口发送结果，`/stm32/ack` 转发 `TYPE=0x00` 的原始应答。
当前 STM32 固件尚未实现控制动作和 ACK 生成，因此发送成功不代表执行成功。
这些通道采用 QoS 0，控制命令不重试，不应设置 retained。
网页按设备显示环境温度、相对湿度和采样新鲜度；数据过期或 MQTT 断线时清空读数。
视频区域保持 16:9、桌面端最大宽度 880px，温湿度卡片位于画面下方，窄屏时自动改为单栏。

配置、完整帧格式、MQTT 示例与验证方法见 [STM32 串口与 MQTT](docs/stm32-uart-mqtt.md)。
