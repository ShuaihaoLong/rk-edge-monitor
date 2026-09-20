# 本地监控屏幕

## 进程和数据流

`rkmon-display` 是独立 C++17 进程，使用固定 LVGL 9.4.0、SDL2/OpenGL，
请求真实 fullscreen 运行在 elf 的 KMSDRM 会话中，不依赖 GNOME/Wayland 桌面。屏幕为 1024×600，视频为左侧
768×432（16:9），右侧显示北京时间、天气、STM32 温湿度及有效状态。

```text
rkmon → MediaMTX 本机 RTSP → MPP 解码/缩放/转 BGRA → 最新 BGRA 帧 → LVGL → SDL2 → KMSDRM
STM32 → rkmon 串口模块 → MQTT → 本地屏幕
ipapi / Open-Meteo → rkmon-weather → 原子替换 JSON 缓存 → 本地屏幕
```

UI 不直接打开相机或串口。视频线程使用 MPP 直接输出 768×432 BGRA，appsink
容量 1、丢旧帧，交接处只保留一帧。LVGL/SDL 操作都在主线程。
该方案增加一次 H.264 解码和 RGB 拷贝，不宣称零拷贝或无额外延迟。

视频超过 2 秒未刷新时隐藏旧画面并显示离线；后端错误或 10 秒无帧时重建连接。
STM32 订阅主题来自 `rkmon.ini` 的 MQTT 配置，仅接受实时 telemetry，检查 schema、
数值范围和来源时间。掉线或超过 `[stm32] stale_timeout_ms` 后隐藏温湿度读数。
不会发送 STM32 控制命令。

## 构建、配置和部署

首次运行 `bash script/prepare-display-deps.sh`，把固定提交
`c016f72d4c125098287be5e83c0f1abed4706ee5` 检出到 `.local/lvgl/source`。
第三方依赖不提交到项目，LVGL MIT 许可随部署包安装。

ARM64 构建默认启用 `RKMON_WITH_LOCAL_DISPLAY`，host 默认关闭。sysroot 需要
SDL2、FreeType、json-c 和 GStreamer app/video 的开发文件；板端需要相应运行库、
MPP 插件和 Noto CJK 字体。构建仍使用 `script/build.sh` 和 Unix Makefiles。

- `rkmon.ini`：复用 `[stream] url`、`[mqtt]` 和 `[stm32]`。
- `display.json`：时区、中文字体路径、天气缓存位置、天气更新间隔和广告目录。
- `rkmon-display.service`：**系统服务**，以 elf 用户运行，使用 KMSDRM 直接输出并独占 DRM。
- `rkmon-weather.service`：系统服务，以 elf 运行，缓存位于 `/var/cache/rkmon-weather/`。

PC 更新统一使用 `bash script/deploy.sh`，包含显示程序、天气程序、配置、许可和服务。
安装后系统服务立即启动显示，并停用 GDM，避免桌面合成器与 KMS 同时争用 DRM。
默认使用 DRM card0，其他用户或设备需调整用户服务环境和 `SDL_KMSDRM_DEVICE_INDEX`。
显示服务接管 DRM，停用 Ubuntu 桌面。未登录图形会话时，天气及主监控仍可运行。

```bash
# 板端管理员
systemctl status rkmon-display
journalctl -u rkmon-display -n 50
systemctl restart rkmon-display
systemctl status rkmon-weather
journalctl -u rkmon-weather -n 50
```

临时退出全屏可按 Escape；手动停止使用 `systemctl stop rkmon-display`。
需要恢复 Ubuntu 桌面时执行 `systemctl disable --now rkmon-display` 和 `systemctl enable --now gdm3`。
不会影响主监控、网页或录像。

## 广告播放

广告视频通过网页的“广告管理”上传到 `/userdata/rkmon-ads/videos/`，后端使用独立
SQLite 保存元数据和播放顺序。上传文件必须是 H.264 MP4，单文件最大 512 MiB。
广告页面可以上移、下移和删除视频；首页按钮可以把本地屏幕切换为广告轮播或实时监控。
默认模式为广告，没有可用广告时自动回退实时监控。API 位于 `/api/ads` 和
`/api/display/mode`，只监听本机并由 Nginx 同源代理。

## 天气

联网后通过 HTTPS `ipapi.co/json/` 获取出口 IP 对应的城市级位置；不是 GPS 定位，
VPN 或运营商出口可能导致城市偏差。定位失败使用合肥（31.8206, 117.2272）。
通过 Open-Meteo 获取温度和天气代码，正常每 15 分钟更新，失败每分钟重试。
UI 显示城市、更新时间和缓存/离线状态，数据源署名显示于底部。

天气失败时保留旧城市与旧读数的对应关系；无缓存时不显示虚构温度。
HTTP 限时和响应大小受限，网络访问独立于 UI；缓存通过同目录临时文件原子替换。
位置服务和天气服务会看到请求出口 IP，API 配额及商用条款以服务商为准：
[ipapi 文档](https://ipapi.co/api/)、[Open-Meteo 文档](https://open-meteo.com/en/docs)。

## 验证边界

性能探针见 `examples/lvgl-video/`。正式程序支持 `--seconds N --capture /tmp/display.bmp`，
用于有限时间运行及应用缓冲区截图；正常服务不指定时间，持续运行。

`tests/display_data_tests.cpp` 覆盖 retained 数据拒绝、数值校验、时间过期、离线和重连状态。
`tests/display_weather_tests.py` 使用本地假响应检查定位回退、失败缓存和原子写入。
测试用假响应不进入正式代码。

当前软件计时只覆盖从 appsink/应用取帧到 SDL 提交；物理屏幕曝光至显示延迟、
触摸准确度和小时级稳定性需单独测量。显示当前只用于信息展示，没有触摸控制操作。
