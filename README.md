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

主机没有相机时，在配置中设置 `[camera] enabled = false`。脚本前台运行，使用 `exec` 保留 PID、信号和退出码；不会自动构建、后台运行或部署。相对命令行路径以调用目录为基准。

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
| `[camera]` | `enabled`、`device`、`width`、`height`、`format`、`fps`、`buffer_count`、`queue_capacity`、`poll_timeout_ms`、`max_consecutive_timeouts` |

节名和键名区分大小写，布尔值使用 `true/false`，注释以 `#` 或 `;` 开头且单独占一行，值不加引号。不支持行尾注释、变量展开或热加载。相对文件路径以 INI 文件所在目录为基准，避免切换工作目录后日志路径变化。

未知节/键、重复节/键、错误类型和越界值会拒绝启动。省略相机节或设置 `enabled=false` 时不装配相机服务；启用时必须提供 `device`。日志级别支持 `trace/debug/info/warn/error/critical/off`；`file` 为空可关闭文件输出，但必须保留控制台或文件中的至少一种。

相机参数范围：宽高 1–16384，FPS 1–1000，驱动 buffer 2–64，应用 queue 1–64，poll 超时 1–60000 ms，连续超时阈值 1–1000。这是配置校验范围，具体分辨率/帧率仍由硬件协商决定。

增加其他设备时，在 `RuntimeConfig` 和配置读取中增加对应节，在 `src/app/application.cpp` 的 `make_service_factory()` 中注册其 `IService` 实现；`main` 和通用生命周期逻辑无需添加设备参数处理。

## 当前 USB 相机能力

已实现 V4L2 单平面 mmap 采集、独立采集线程、只读自有帧数据、有界队列、故障退出和优雅关闭。仓库 INI 默认配置 SYD USB Camera，MJPEG 1920×1080@30，使用稳定的 `/dev/v4l/by-id/usb-SYD_USB_Camera_200901010001-video-index0` 路径。

实机 `/dev/video21` 为视频节点，`/dev/video22` 为元数据节点；编号可能变化。相机声明 MJPEG 1080p@30、YUYV 1080p@5 / 720p@10 / 480p@30。启动日志记录协商结果，退出日志记录采集 FPS、应用队列丢帧和超时数。

当前正式应用只生产帧并保留最新队列内容，尚未接入业务消费者，因此队列丢帧增加属于预期。不会显示、保存或推送视频；连续读取超时达到配置阈值或设备错误，会关闭队列并让应用退出。自动重连尚未实现。

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

后续 1080p@30 链路需要 `MJPEG 解码 → 原始图像 → RGA / RKNN / MPP 编码`。当前功能与框架检查见 [框架检查记录](docs/framework-review.md)，历史采集验证见 [相机验证记录](docs/usb-camera-validation.md)。
