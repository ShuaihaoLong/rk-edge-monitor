#!/usr/bin/env bash
# 用途：停止板端 rkmon systemd 服务，可从任意目录调用。
# 示例：sudo bash script/stop.sh
# 参数：--help 显示说明；无其他参数。
# 环境：无专用环境变量。前提：已通过 install-monitor.sh 安装 rkmon.service，并以 root 运行。
# 输出：服务停止结果；日志保留在 journalctl 中。
# 副作用：停止相机采集、编码、AI、MQTT 和 STM32 相关主程序；不停止其他后台服务或 GDM。
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

systemctl stop rkmon.service
printf 'rkmon 已停止。\n'
