# 开发与构建

## 前置条件

主机使用 CMake + Unix Makefiles + GNU Make。纯逻辑和主机测试使用 x86_64 环境；RK3588 目标需要 ARM64 交叉 GCC、板端 sysroot，以及板端 GStreamer/MPP/RGA/RKNN 运行库。交叉编译和 sysroot 准备步骤见 [交叉编译说明](cross-compile.md)。

源码保持 C++17；线程使用 `std::thread`、显式停止和 `join`。主机构建不链接 ARM 专用库，ARM64 构建默认开启硬件后端。

## 常用命令

```bash
bash script/build.sh --target host --test
bash script/build.sh --target rk3588 --jobs 4
ctest --test-dir build/host --output-on-failure
bash script/start.sh --target host --config /tmp/rkmon.ini
bash script/deploy.sh --dry-run
bash script/deploy.sh --skip-build
```

`start.sh` 前台运行并使用 `exec` 保留 PID、信号和退出码；脚本可从任意目录调用。ARM64 程序必须在板端运行。

## CMake 选项

根工程加入 `core`、`camera`、`media`、`ai`、`mqtt`、`stm32`、`app` 和可选测试。媒体和 AI 的硬件能力由以下选项控制：

- `RKMON_WITH_GSTREAMER`：GStreamer 解码和 RTSP 发布；aarch64 默认开启。
- `RKMON_WITH_RGA`：RGA 帧复制或 AI 预处理；aarch64 默认开启。
- `RKMON_WITH_RKNN`：RKNN Runtime 和 YOLOv8 后处理；aarch64 默认开启。
- `RKMON_BUILD_TESTS`：构建测试；默认开启。

不同目标使用不同 build 目录，不要混用 CMake cache。切换 sysroot 时建议创建新的 build 目录。

## 测试层级

- CTest：Core、配置、模块装配、相机、媒体、AI、MQTT、STM32 和启动脚本测试。
- Python 单测：录像索引、事件聚合、恢复和清理。
- 板端 probe：真实 V4L2、GStreamer/MPP/RGA/RKNN 和 RTSP 能力。
- 集成/浏览器测试：录像服务、MediaMTX、Nginx 和网页回放。

ARM64 测试通常只交叉编译，不在主机执行；需要板端硬件的测试按 [Camera](modules/camera.md) 和 [Media](modules/media.md) 中的前置条件执行。

## 代码与文档边界

C++ 正式实现放在 `src/`，测试和探针放在 `tests/` 或 `examples/`。Python 录像服务属于部署运行时，不加入根 CMake；它通过 `script/package-monitor.sh` 打包并由 systemd 启动。源码和脚本约束见根目录 [AGENTS.md](../AGENTS.md)。
