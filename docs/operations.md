# 运行与运维

## 部署链路

PC 端统一入口是 `script/deploy.sh`：检查 SSH 和板端依赖，构建或复用 ARM64 程序，调用 `package-monitor.sh` 打包，上传到板端独立临时目录，执行 root 安装脚本，检查服务和 HTTP，最后清理临时目录。

```bash
bash script/deploy.sh --dry-run
bash script/deploy.sh --skip-build
bash script/deploy.sh --host elf@192.168.100.11
bash script/deploy.sh --skip-build --display-recording-only
```

部署包由 `script/package-monitor.sh` 复制 C++ 二进制、MediaMTX、Python 录像服务、网页、配置、systemd 文件和离线 deb 包。板端安装到 `/opt/rkmon`；录像和 SQLite 保留在 `/userdata/rkmon-video`，升级不会覆盖录像目录。

`--display-recording-only` 复用同一构建/打包/安装链路，只更新显示程序、录像索引程序及 `reserve_percent` 配置，并重启这两个服务；保留板端其他录像参数、主程序 AI/码率配置、天气和网络设置，不重启采集及 MediaMTX。回滚文件保存在 `/opt/rkmon-display-recording-backup.*`。新空间策略会按需删除最旧的已完成录像，直至达到空闲目标。

服务控制脚本随部署安装到 `/opt/rkmon/script/`，包括 `start.sh`、`start_all.sh`、`stop.sh` 和 `stop_all.sh`。

广告管理页面为 `http://<board-ip>:9000/ads.html`。上传、排序和删除不会进入录像清理目录；
显示模式通过首页的“播放广告/实时监控”按钮切换。

## 服务关系

| 服务 | 作用 | 主要配置 |
| --- | --- | --- |
| `rkmon` | 相机、视频、AI、MQTT、STM32 | `/opt/rkmon/config/rkmon.ini` |
| `rkmon-recording` | 录像索引、事件和回放 API | `/opt/rkmon/config/recording.ini` |
| `mediamtx` | RTSP、WebRTC、录像、Playback | `/opt/rkmon/config/mediamtx.yml` |
| `mosquitto` | MQTT broker | `/etc/mosquitto/conf.d/rkmon.conf` |
| `nginx` | 网页和同源代理 | `/etc/nginx/sites-available/rkmon` |

主要入口是 `http://<board-ip>:9000/`。Nginx 将 `/api/recordings` 和 `/api/playback` 转给 `rkmon-recording`，将 `/recording-media/get` 转给 MediaMTX Playback；录像文件路径使用 internal alias，不直接暴露目录。

## 日常检查

```bash
systemctl status rkmon rkmon-recording mediamtx mosquitto nginx
journalctl -u rkmon -u rkmon-recording -u mediamtx -f
curl --fail http://127.0.0.1:9000/api/recordings/status
sudo nginx -t
```

服务生命周期脚本：

```bash
sudo bash script/start.sh       # 重启 rkmon
sudo bash script/stop.sh        # 停止 rkmon
sudo bash script/start_all.sh   # 停止 GDM，启动全部后台服务和本地屏幕
sudo bash script/stop_all.sh    # 停止全部后台服务，恢复 GDM 桌面
```

`start_all.sh` 和 `stop_all.sh` 会改变本地显示模式；切换前应确保没有未保存的桌面操作。

部署会通过 NetworkManager 将 Wi-Fi 路由 metric 设为 50、有线管理网设为 600，
避免管理网成为公网天气请求的默认出口。

录像状态接口会返回索引服务状态、最后扫描时间、最后事件时间和磁盘空间。录像服务由 `elf` 运行，MediaMTX 也以 `elf` 运行；录像目录需要该用户可写。

## 故障处理

- 服务反复重启：先查看对应 `journalctl`，再检查配置和动态库，不要先删除录像目录。
- 录像列表为空：检查 `rkmon-recording`、`/userdata/rkmon-video`、`ffprobe` 和 MediaMTX segment hook。
- 回放失败：检查 MediaMTX Playback 是否监听 `127.0.0.1:9996`，以及 Nginx 配置是否通过 `nginx -t`。
- 磁盘不足：录像服务按保留天数、总容量和预留空间清理旧分段；不要手工删除正在写入的文件。
- 部署失败：部署临时目录只用于本次传输；若 sudo 生成 root 文件，`deploy.sh` 会用 sudo 兜底清理。

## 安全边界

当前配置面向可信局域网，没有登录、TLS 或公网认证。MediaMTX 的 RTSP、Playback 和 WebRTC 发布入口绑定本机，网页仅通过 Nginx 暴露。不要把 9000 端口直接映射到不可信网络；更换板卡地址时同步修改 MediaMTX WebRTC allow-origin 和 additional-host 配置。

## 本地屏幕

新增 LVGL 本地屏幕随 `script/deploy.sh` 更新，使用系统级 KMSDRM 服务独占 DSI/DRM 输出；部署会停用 GDM，避免桌面合成器争用显示设备。
系统服务 `rkmon-display` 显示视频、时间、天气及 STM32 温湿度，系统服务
`rkmon-weather` 更新天气缓存。配置、日志、停止方式和网络失败行为见
[Display 模块](modules/display.md)。板端需要 SDL2、FreeType、json-c、MPP 插件和 Noto CJK 字体。
