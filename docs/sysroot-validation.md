# Sysroot 获取与验证记录

- 本地目录：`/home/lsh/rk-edge-monitor/.local/sysroots/rk3588`
- 来源：`elf@192.168.100.11`，Ubuntu 22.04.5 / arm64。
- 同步前空间：WSL 可用约 952 GiB；Windows C 盘可用约 81 GiB。
- 实际占用：`du -sh` 显示 5.4G。
- 同步目录：`/usr/include`、`/usr/lib`、`/usr/share/pkgconfig`。
- 排除：`/usr/lib/ssl/private`、`/usr/lib/cups/backend/cups-brf`、`/usr/lib/cups/backend/implicitclass`。
- 最终 rsync 返回 0；未修改板端文件或安装软件包。
- 本地创建 `lib -> usr/lib`；转换 114 个绝对软链接；检查无软链接逃逸。
- 144 个缺少目标的链接保留并记录，详见 `.local/sysroots/rk3588-link-report.json`。当前快照面向视频编译，不是完整 rootfs。
- GCC 11.4.0 + CMake Unix Makefiles 编译链接 `examples/gst-smoke` 成功。
- GStreamer 1.20.3 / gstreamer-app 1.20.1 从 sysroot 找到；构建输出中的 GStreamer/GLib 头文件与链接库均来自 sysroot。
- 产物：`build/rk3588-gst-smoke-make/gst-smoke`。
- 本次未部署或运行板端验证程序，未测试摄像头。

后续终端使用：

```bash
export RK3588_SYSROOT=/home/lsh/rk-edge-monitor/.local/sysroots/rk3588
```
