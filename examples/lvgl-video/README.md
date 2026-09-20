# LVGL 视频显示探针

独立进程读取本机 `rtsp://127.0.0.1:8554/camera`，MPP 解码并缩放为
768×432 BGRx，LVGL 9.4.0 软件绘制后由 SDL2/OpenGL 提交至现有 Wayland 桌面。
不打开相机、不停止 rkmon 或录像。测试会全屏覆盖当前桌面，结束后恢复。

依赖：板端 SDL2、GStreamer app/video、厂商 mppvideodec；构建使用项目 ARM64
sysroot、交叉 GCC 和 GNU Make。LVGL 固定提交为
`c016f72d4c125098287be5e83c0f1abed4706ee5`（v9.4.0）。

```bash
git clone --depth 1 --branch v9.4.0 --filter=blob:none --sparse https://github.com/lvgl/lvgl.git .local/lvgl/source
git -C .local/lvgl/source sparse-checkout init --cone
git -C .local/lvgl/source sparse-checkout set src env_support
RK3588_SYSROOT="$PWD/.local/sysroots/rk3588" cmake -S examples/lvgl-video -B build/lvgl-probe \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/rk3588-linux.cmake" \
  -DLVGL_SOURCE_DIR="$PWD/.local/lvgl/source"
make -C build/lvgl-probe -j4
```

将程序复制到板端临时目录，在 elf 已登录的图形会话中运行：

```bash
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
TZ=Asia/Shanghai ./lvgl-video-probe --seconds 60 --capture /tmp/probe.bmp
```

`--help` 查看参数。Escape/SIGTERM 退出。截图是应用合成缓冲区，不能证明屏幕
物理扫描或触摸有效。FPS 统计首帧至末帧之间的软件提交次数；render 时间从取到
appsink 帧后计时，到 SDL_RenderPresent 返回为止，不包含采集、编码、RTSP、硬解
及物理扫描，不能当成端到端延迟。CPU 为整个进程累计 CPU 时间除运行时间，
100% 相当于一个逻辑核。gaps_over_100ms 是相邻视频提交间隔超过 100 ms 的次数。

可在板端运行 `python3 tests/local_display_metrics.py --seconds 60` 读取各进程
CPU/RSS，分别在没有 UI 和有 UI 时采样。测试日志和截图只存临时目录。
