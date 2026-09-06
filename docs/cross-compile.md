# WSL2 Ubuntu 22.04 → RK3588 Ubuntu 22.04

当前主机为 x86_64 Ubuntu 22.04.5，交叉 GCC 11.4.0、CMake 和 Make 已可用。板端 sysroot 已下载至 `.local/sysroots/rk3588`，实际占用约 5.4 GiB。已完成软链接处理，并通过 GStreamer 示例的 Unix Makefiles 交叉编译和链接；尚未执行本次构建产物的板端运行验证。安装步骤保留供新环境参考，当前板端无需重新安装开发包。

代码在 WSL2 Linux 文件系统中编辑和编译；VSCode 使用 WSL 窗口打开仓库。SSH 用于部署、运行和调试。VSCode SSH 窗口中的终端位于板端，不能把它误认为 WSL 交叉编译终端。

## 脚本快捷入口

所有脚本开头包含中文使用说明，也支持 `--help`。在项目根目录执行：

```bash
# 使用现有 sysroot，通过 CMake + Makefile 构建验证程序
bash script/smoke-build.sh --jobs 4

# 仅查看同步计划，不下载或修改 sysroot
bash script/sync-sysroot.sh --dry-run

# 已有 rk3588 快照时，下载到一个尚不存在的新目录
bash script/sync-sysroot.sh --dest "$PWD/.local/sysroots/rk3588-new"

# 使用新快照构建；建议切换 SDK 时同时使用新的构建目录
RK3588_SYSROOT="$PWD/.local/sysroots/rk3588-new" \
SMOKE_BUILD_DIR="$PWD/build/rk3588-gst-smoke-new" \
bash script/smoke-build.sh
```

同步脚本不覆盖已有快照，不保存 SSH 密码；成功后自动调用 `fix-sysroot-links.py`。同步或检查失败会保留暂存目录并返回非零状态。它会检查 WSL 目标文件系统至少有 8 GiB 可用空间；WSL 动态磁盘所在 Windows 分区的空间仍需单独检查。

## 1. WSL 安装工具

```bash
sudo apt update
sudo apt install build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  cmake make pkg-config rsync openssh-client gdb-multiarch file

aarch64-linux-gnu-g++ --version
cmake --version
make --version
ssh elf@192.168.100.11
```

首次 SSH 连接确认板卡主机身份后接受指纹，交互输入密码，不将密码放进脚本。

## 2. 板端确认架构和开发依赖

以下在板端执行。uname 预期 aarch64，dpkg 预期 arm64；如果不符合，暂停采用本工具链。

```bash
uname -m
dpkg --print-architecture
cat /etc/os-release
ldd --version
gst-inspect-1.0 --version
```

记录已装 GStreamer 包版本和来源。若为厂家定制版本，先确认开发包能与其配套，再安装；不要为了处理依赖冲突移除厂商多媒体包。

```bash
apt-cache policy libgstreamer1.0-0 libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-0 libgstreamer-plugins-base1.0-dev
```

标准 Ubuntu 软件包环境可以执行：

```bash
sudo apt update
sudo apt install libc6-dev libstdc++-11-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  v4l-utils rsync gdbserver

pkg-config --modversion gstreamer-1.0 gstreamer-app-1.0
v4l2-ctl --list-devices
gst-inspect-1.0 v4l2src
```

根据设备列表选择真正的视频采集节点，以下 video0 只是示例：

```bash
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
```

USB ID 0bda:d576 不能替代上述格式查询。之后根据实际 MJPEG/YUYV/H.264 格式选择采集及解码 pipeline。

## 3. WSL 获取板端 sysroot

sysroot 是板端开发头文件、链接库与依赖元数据的本地快照，不是完整可启动系统。必须在安装板端开发包之后同步。不能用 WSL 的 x86_64 GStreamer 库代替 ARM64 库。

当前快照已完成，无需重复执行。下列命令用于在新环境创建快照：

```bash
cd /home/lsh/rk-edge-monitor
export RK3588_SYSROOT="$PWD/.local/sysroots/rk3588"
mkdir -p "$RK3588_SYSROOT"
rsync -aR --no-owner --no-group --stats \
  --exclude=/usr/lib/ssl/private \
  --exclude=/usr/lib/cups/backend/cups-brf \
  --exclude=/usr/lib/cups/backend/implicitclass \
  elf@192.168.100.11:/./usr/include :/./usr/lib :/./usr/share/pkgconfig \
  "$RK3588_SYSROOT/"
# 仅在 rsync 成功（退出码 0）后执行：
python3 script/fix-sysroot-links.py "$RK3588_SYSROOT"
```

保留软链接，不使用 `-L`：板端 HDF5 目录存在指回自身的链接，全量解引用会递归失败。排除项为 SSL 私钥目录及两个普通用户无法读取的 CUPS 打印后端，与视频交叉编译无关。实际传输常规文件 71,169 个，约 5.53 GB；最后一次同步退出码为 0。

脚本建立 `lib -> usr/lib`，将绝对链接改为 sysroot 内部的相对链接，检查关键文件并在 sysroot 同级目录生成 `rk3588-link-report.json`。本次转换 114 个绝对链接，没有链接逃逸到主机目录。保留了 144 个缺少目标的链接，涉及未同步的 alternatives、Java 配置、系统程序等；这是用于视频开发的部分文件系统快照，不是完整 rootfs。GStreamer 链接验证已通过，后续引入 BLAS/MPI 等其他依赖时需补齐对应目标，不应宣称所有板端依赖均可用。

如果板端依赖安装在 /usr/local 或 /opt，需根据实际路径另行同步并配置搜索路径；当前工具链默认 Ubuntu 标准路径。更新板卡依赖后应制作新的干净快照并使用新的构建目录，避免保留旧库造成混用。

## 4. 交叉编译 GStreamer 最小验证程序

仓库提供 cmake/toolchains/rk3588-linux.cmake 和 examples/gst-smoke。工具链限制库、头文件和 pkg-config 元数据从 sysroot 查找；构建工具仍在 WSL 执行。

在同一个 WSL 终端执行（新终端需重新 export RK3588_SYSROOT）：

```bash
cd /home/lsh/rk-edge-monitor
export RK3588_SYSROOT="$PWD/.local/sysroots/rk3588"
cmake -S examples/gst-smoke -B build/rk3588-gst-smoke-make -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/rk3588-linux.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build/rk3588-gst-smoke-make -j"$(nproc)" VERBOSE=1

file build/rk3588-gst-smoke-make/gst-smoke
aarch64-linux-gnu-readelf -l build/rk3588-gst-smoke-make/gst-smoke
scp build/rk3588-gst-smoke-make/gst-smoke elf@192.168.100.11:/tmp/gst-smoke
ssh elf@192.168.100.11 'ldd /tmp/gst-smoke && /tmp/gst-smoke'
```

这里使用独立的 `build/rk3588-gst-smoke-make` 目录，避免与之前可能生成的其他构建系统缓存冲突。Makefile 由 CMake 生成，不手动编辑。

验收：file 显示 ARM aarch64；readelf 显示目标动态加载器（通常 /lib/ld-linux-aarch64.so.1）；板端 ldd 没有 not found；程序输出 `PASS: received one test frame`。它验证交叉链接、板端 GStreamer 插件加载及 appsink 收帧，不验证摄像头或 MPP。

不能在 WSL 直接执行 ARM 程序。出现 GLIBC/GLIBCXX 版本错误时核对板端系统、交叉 GCC 和 sysroot，不要通过随意替换板端 libc/libstdc++ 处理。

## 5. 视频优先的后续顺序

1. 测试源 → appsink 验证工具链。
2. USB 摄像头 → appsink，确认实际 caps、帧大小、时间戳及采集 FPS。
3. 加入有界队列和慢消费者验证。
4. 检查板端 MPP/GStreamer 编码插件，完成 H.264 与 RTSP。
5. 再加入 AI 分支；串口阶段后移。

官方参考：
- Ubuntu 22.04 ARM64 交叉 C++ 编译器：https://packages.ubuntu.com/en/jammy/amd64/g%2B%2B-aarch64-linux-gnu
- CMake Linux 交叉编译工具链：https://cmake.org/cmake/help/v3.31/manual/cmake-toolchains.7.html
