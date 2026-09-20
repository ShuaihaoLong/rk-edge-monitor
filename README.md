# RK3588 边缘智能监控

这是一个面向 RK3588 的单路摄像头边缘监控工程，包含 USB 摄像头采集、硬件解码、AI 检测、H.264/RTSP 推流、WebRTC 网页监控、录像回放、MQTT 状态和 STM32 串口接入。

当前系统由以下运行单元组成：

- `rkmon`：C++17 主程序，负责相机、视频、AI、MQTT 和 STM32。
- MediaMTX：RTSP、WebRTC、录像切片和 Playback API。
- `rkmon-recording`：Python 录像索引、事件聚合、回放 API 和容量清理服务。
- Nginx：网页、同源 API、WebRTC 和录像文件代理。
- Mosquitto：MQTT broker。

完整模块说明见 [docs/README.md](docs/README.md)。开发约束见 [AGENTS.md](AGENTS.md)。

## 1. 前置条件

### 主机

主机需要 Bash、CMake、GNU Make、C++17 编译器、Git、Python 3、curl、tar 和 SSH 客户端。部署时还需要可用的 SSH 密钥。

```bash
sudo apt update
sudo apt install build-essential cmake make python3 curl tar openssh-client file
```

### RK3588 交叉编译

交叉编译需要 ARM64 GCC/G++ 和板端 sysroot：

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  pkg-config rsync gdb-multiarch
export RK3588_SYSROOT="$PWD/.local/sysroots/rk3588"
```

sysroot 应来自目标板的 ARM64 头文件、库和 pkg-config 元数据，不能用主机 x86_64 库替代。详细同步方法见 [docs/cross-compile.md](docs/cross-compile.md)。

### 板端运行环境

板端应为 `aarch64`，并预装：

- Nginx、curl、Python 3、SQLite、ffprobe。
- Rockchip GStreamer/MPP/RGA 运行库和插件。
- RKNN Runtime、NPU 驱动及模型所需设备权限。
- Mosquitto 及其服务用户权限。
- 摄像头设备权限、`/dev/ttyS9` 串口权限和 `/userdata` 可写空间。

MediaMTX 安装包、AI 模型和 Mosquitto ARM64 离线 deb 包由项目的本地准备流程提供，部署脚本会校验固定文件摘要。

### 部署前需要准备的文件

下面的文件只用于本地构建和打包，保存在执行构建和部署的电脑上即可。首次部署前，在项目根目录执行准备脚本，脚本会自动下载并校验固定版本的文件：

```bash
python3 script/prepare-ai-probe.py
bash script/prepare-mqtt-deps.sh
```

准备完成后，目录中至少应有以下文件：

| 文件或目录 | 内容 | 来源/准备方式 |
| --- | --- | --- |
| `.local/sysroots/rk3588/` | ARM64 头文件、库和 pkg-config 元数据 | 从目标板同步，见 [交叉编译说明](docs/cross-compile.md)；只做主机构建时可不准备 |
| `.local/ai-stage4/yolov8n.rknn` | RK3588 YOLOv8n INT8 模型 | `prepare-ai-probe.py` 自动从 Mixtile 下载并校验 |
| `.local/ai-stage4/zoo/` | 固定提交的 RKNN Model Zoo 后处理、头文件和标签 | `prepare-ai-probe.py` 自动从 GitHub 下载并校验 |
| `.local/downloads/mediamtx_v1.21.0_linux_arm64.tar.gz` | MediaMTX v1.21.0 ARM64 压缩包 | 从 [MediaMTX v1.21.0 Releases](https://github.com/bluenviron/mediamtx/releases/tag/v1.21.0) 下载 |
| `.local/downloads/mosquitto-arm64/` | Mosquitto 及其 ARM64 依赖 deb 包 | `prepare-mqtt-deps.sh` 自动从 Ubuntu Ports 镜像下载并校验 |

`prepare-ai-probe.py` 下载的模型固定 SHA256 为：

```text
defa25aea179be4da5c5c5826e0be26833b9f818f86b6519620f52f6df3b6a17
```

`prepare-mqtt-deps.sh` 会准备以下 deb 包：

```text
libdlt2_2.18.6-2_arm64.deb
libev4_4.33-1_arm64.deb
libmosquitto1_2.0.11-1ubuntu1.2_arm64.deb
libwebsockets16_4.0.20-2ubuntu1.1_arm64.deb
mosquitto_2.0.11-1ubuntu1.2_arm64.deb
```

只检查本地文件而不联网：

```bash
python3 script/prepare-ai-probe.py
bash script/prepare-mqtt-deps.sh
```

只检查本地文件而不联网：

```bash
python3 script/prepare-ai-probe.py --offline
bash script/prepare-mqtt-deps.sh --offline
```

MediaMTX 压缩包的默认位置为 `.local/downloads/mediamtx_v1.21.0_linux_arm64.tar.gz`。`.local/mediamtx-v1.21.0/` 如果存在，只是解压缓存，当前部署流程不读取它，可以删除。

## 2. 获取和检查工程

```bash
git submodule update --init --recursive
cd /home/lsh/rk-edge-monitor
```

## 3. 构建和测试

### 主机构建与 CTest

主机构建不启用 ARM 专用 GStreamer、RGA 和 RKNN 后端，适合运行纯逻辑测试：

```bash
bash script/build.sh --target host --jobs 4 --test
ctest --test-dir build/host --output-on-failure
```

### ARM64 构建

```bash
bash script/build.sh --target rk3588 --jobs 4
```

产物位于：

```text
build/rk3588/src/app/rkmon
build/rk3588/tests/
```

ARM64 二进制不能在 x86_64 主机直接运行，必须部署到 RK3588 板端。主机和 ARM64 使用不同 build 目录，切换 sysroot 时建议使用新的 build 目录。

### 单独启动

```bash
# 主机测试配置
bash script/start.sh --target host --config /tmp/rkmon-host.ini

# 使用默认 ARM64 配置
bash script/start.sh

# 指定程序和配置
bash script/start.sh --binary /path/to/rkmon --config /path/to/rkmon.ini
```

`start.sh` 前台运行并使用 `exec` 保留进程 PID、信号和退出码；不会自动构建、后台运行或部署。主机没有摄像头时，应同时关闭 `[camera]` 和 `[video]`。

## 4. 配置

主程序配置为 [config/rkmon.ini](config/rkmon.ini)，录像服务配置为 [config/recording.ini](config/recording.ini)。配置在启动时读取，修改后需要重启服务。

主要配置节：

| 节 | 作用 |
| --- | --- |
| `[app]` | 控制邮箱容量 |
| `[logging]` | 控制台、文件、级别和日志轮转 |
| `[camera]` | V4L2 设备、分辨率、格式、FPS 和采集队列 |
| `[video]` | JPEG 到 NV12 的硬件解码 |
| `[stream]` | H.264 编码、RTSP 地址、码率、GOP 和 OSD |
| `[ai]` | 模型、RGA/RKNN、推理频率、结果和事件 Socket |
| `[mqtt]` | broker、设备 ID、状态发布和保活 |
| `[stm32]` | UART 设备、波特率、协议超时和重连 |
| `[recording]` | 录像目录、索引 API、保留天数和磁盘预留空间 |

节名和键名区分大小写；不支持变量展开或行尾注释；相对路径以配置文件目录为基准。未知键、重复键、错误类型和越界值会拒绝启动。完整字段和范围见 [docs/configuration.md](docs/configuration.md)。

## 5. 一键部署

确认 `.local/downloads/` 中已有固定版本的 MediaMTX 包、AI 模型和 Mosquitto ARM64 依赖后执行：

```bash
# 只查看流程，不构建、不上传、不修改板端
bash script/deploy.sh --dry-run

# 完整构建、打包、上传、安装和健康检查
bash script/deploy.sh

# 复用已有 ARM64 构建产物
bash script/deploy.sh --skip-build

# 指定板端和并行构建数
bash script/deploy.sh --host elf@192.168.100.11 --jobs 4
```

部署流程：

1. SSH 检查板端架构和基础命令。
2. 构建 ARM64 `rkmon`，或复用已有产物。
3. 校验 MediaMTX、模型和离线依赖摘要。
4. 打包程序、网页、配置、Python 录像服务和 systemd 文件。
5. 上传到板端独立临时目录。
6. 以 root 安装并备份旧配置。
7. 重启服务并检查 systemd 状态和 HTTP 接口。
8. 清理上传临时目录。

安装位置和数据目录：

```text
/opt/rkmon                         程序、网页、配置和 Python 服务
/userdata/rkmon-video             MP4 录像和 index.sqlite3，不随升级删除
/opt/rkmon-backup.*               仅保留一个上一版本的旧文件备份
```

部署会短暂停流，覆盖 `/opt/rkmon` 下的项目配置和网页，但不会覆盖 `/userdata/rkmon-video`。每次成功部署都会用当前版本生成一个新的上一版本备份，并删除更早的备份；如果备份生成失败，则保留已有备份。板端 `sudo` 可能交互询问密码；密码不会写入脚本或日志。

## 6. 运行和访问

部署完成后检查：

```bash
systemctl status rkmon rkmon-recording mediamtx mosquitto nginx
journalctl -u rkmon -u rkmon-recording -u mediamtx -f
curl --fail http://127.0.0.1:9000/api/recordings/status
```

网页入口：

```text
http://<板端 IP>:9000/
```

网页提供实时 WebRTC 画面、检测状态、录像日期、时间轴、事件定位、录像下载和回放。录像由 MediaMTX 按约 1 分钟切片，Python 服务负责索引和事件关联，录像默认保留 7 天并预留至少 1 GiB 磁盘空间。

常用维护命令：

```bash
sudo systemctl restart rkmon
sudo systemctl restart rkmon-recording
sudo systemctl restart mediamtx
sudo nginx -t
journalctl -u rkmon -u rkmon-recording -u mediamtx -f
```

更换板卡 IP 时，需要同步修改 `config/mediamtx.yml` 中的 WebRTC origin 和 additional host，并按实际相机、串口和 MQTT 设备 ID 修改 INI。

## 7. 模块和代码入口

- `src/app`：配置、信号、服务工厂和主程序生命周期。
- `src/core`：BoundedQueue、Mailbox、IService、ServiceManager 和日志。
- `src/camera`：V4L2 mmap 采集和采集队列。
- `src/media`：GStreamer/MPP 解码、NV12、OSD、编码和 RTSP。
- `src/ai`：RGA/RKNN 预处理、推理、YOLOv8 后处理和检测快照。
- `src/mqtt`：Mosquitto 连接、状态和消息发布。
- `src/stm32`：UART 协议、CRC、DHT11 数据和 MQTT 桥接。
- `src/recording`：Python 录像索引、事件聚合、回放 API 和清理。
- `web`：原生 HTML/CSS/JavaScript 实时监控和录像回放页面。
- `config`：INI、MediaMTX、Nginx、Mosquitto 和 systemd 配置。

模块级说明见 [docs/modules/](docs/modules/)。

## 8. 重要限制

- 当前默认面向可信局域网，没有登录、TLS、TURN 或公网认证。
- ARM64 构建成功不等于板端硬件插件、相机格式和模型运行时可用。
- 队列有界，慢消费者会丢旧视频帧；这不等于驱动丢帧。
- 当前短时性能数据不代表长期稳定性、零拷贝或严格 30 FPS。
- 摄像头物理拔插、断电恢复、多路摄像头和长时间压力仍需按实际部署环境验证。
- STM32 当前固件尚未实现具体控制动作和业务 ACK，串口 `sent` 只表示数据写入驱动。
- 录像回放依赖 MediaMTX、ffprobe、Nginx Range 文件传输和 `/userdata` 空间，相关服务异常会使列表或回放不可用。
