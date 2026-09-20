#!/usr/bin/env bash
# 用途：停止 Ubuntu 桌面并启动/重启 RKMon 全部板端后台服务，可从任意目录调用。
# 示例：sudo bash script/start_all.sh
# 参数：--help 显示说明；无其他参数。
# 环境：无专用环境变量。前提：已通过 install-monitor.sh 安装 systemd 服务，并以 root 运行。
# 输出：各服务的 systemd 状态；日志使用 journalctl 管理。
# 副作用：停用并停止 gdm3，独占 DRM；重启 rkmon、显示、录像、媒体、MQTT、天气和 Nginx 服务。
set -euo pipefail

if (($#)); then
    case "$1" in
        --help|-h)
            sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "错误：未知参数 $1" >&2
            exit 2
            ;;
    esac
fi
[[ $EUID == 0 ]] || { echo '错误：请以 root 运行。' >&2; exit 1; }

systemctl disable --now gdm3.service 2>/dev/null || true
systemctl restart mosquitto.service mediamtx.service rkmon-recording.service \
    rkmon.service rkmon-weather.service rkmon-display.service nginx.service

for service in mosquitto mediamtx rkmon-recording rkmon rkmon-weather rkmon-display nginx; do
    systemctl is-active --quiet "$service.service"
done
printf '全部后台服务已启动，GDM 已停用。\n'
