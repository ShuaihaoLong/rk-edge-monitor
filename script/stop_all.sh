#!/usr/bin/env bash
# 用途：停止 RKMon 全部板端后台服务并恢复 Ubuntu GNOME 桌面，可从任意目录调用。
# 示例：sudo bash script/stop_all.sh
# 参数：--help 显示说明；无其他参数。
# 环境：无专用环境变量。前提：已通过 install-monitor.sh 安装 systemd 服务，并以 root 运行。
# 输出：各服务的停止和 GDM 恢复结果；日志使用 journalctl 管理。
# 副作用：停止显示、相机、录像、媒体、MQTT、天气和 Nginx 服务，然后启用并启动 gdm3。
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

systemctl stop rkmon-display.service rkmon.service rkmon-weather.service \
    rkmon-recording.service mediamtx.service mosquitto.service nginx.service
systemctl enable --now gdm3.service
systemctl is-active --quiet gdm3.service
printf '全部后台服务已停止，GDM 已恢复。\n'
