#!/usr/bin/env bash
# 用途：在 RK3588 上安装打包好的监控程序、网页和 systemd 服务。
# 示例：sudo bash /tmp/rkmon-deploy/script/install-monitor.sh
# 参数：--help 显示说明；无环境变量。固定运行用户 elf，安装目录 /opt/rkmon。
# 前提：ARM64、Python3/SQLite、ffprobe、Nginx 和 Rockchip GStreamer/RKNN，以 root 运行；
#       包内含 bin/、config/、web/、models/、licenses/ 和 Mosquitto 软件包，无需联网。
# 输出：/opt/rkmon；录像/SQLite 在 /userdata/rkmon-video（部署保留）；备份 /opt/rkmon-backup.*。
# 副作用：更新服务及 9000 端口站点，启用开机启动并重启服务；不修改其他 Nginx 站点。
set -euo pipefail
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
    exit 0
fi
[[ $# == 0 ]] || { echo '错误：不支持的参数' >&2; exit 2; }
[[ $EUID == 0 && $(uname -m) == aarch64 ]] || { echo '错误：请在 ARM64 板卡上以 root 运行' >&2; exit 1; }
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
for file in lib/recording/service.py lib/recording/notify.py config/recording.ini config/systemd/rkmon-recording.service web/recordings.html web/recordings.js web/recordings.css bin/rkmon bin/mediamtx config/rkmon.ini config/mediamtx.yml config/nginx-monitor.conf config/mosquitto-rkmon.conf config/systemd/rkmon.service config/systemd/mediamtx.service web/index.html web/player.js web/vendor/reader.js web/vendor/mqtt.min.js web/detections.js web/device-status.js models/yolov8n.rknn models/coco_80_labels_list.txt licenses/RKNN-Model-Zoo-LICENSE packages/SHA256SUMS; do
    [[ -f $source_dir/$file ]] || { echo "错误：缺少 $file" >&2; exit 1; }
done
id elf >/dev/null
command -v nginx >/dev/null
command -v ffprobe >/dev/null
python3 -c 'import sqlite3, zoneinfo; zoneinfo.ZoneInfo("Asia/Shanghai")'
python3 -m py_compile "$source_dir/lib/recording/"{service,notify}.py
(
    cd "$source_dir/packages"
    sha256sum --check --strict SHA256SUMS
)
# 软件包来自 Ubuntu 22.04 ports，先安装 broker 再变更运行中的项目服务。
dpkg -i "$source_dir/packages/"*.deb
# 先验证媒体配置和动态库，避免安装不完整的包后停止现有服务。
"$source_dir/bin/mediamtx" --validate-conf "$source_dir/config/mediamtx.yml"
if ldd "$source_dir/bin/rkmon" | grep -q 'not found'; then
    echo '错误：rkmon 缺少动态库' >&2
    exit 1
fi
backup_dir=$(mktemp -d /opt/rkmon-backup.XXXXXXXX)
for path in /opt/rkmon /etc/nginx/sites-available/rkmon /etc/nginx/sites-enabled/rkmon /etc/mosquitto/conf.d/rkmon.conf /etc/systemd/system/rkmon.service /etc/systemd/system/mediamtx.service /etc/systemd/system/rkmon-recording.service; do
    if [[ -e $path || -L $path ]]; then cp -a --parents "$path" "$backup_dir/"; fi
done
# 新备份已完整生成后，只保留它作为下一次回滚点；备份失败时保留旧备份。
for old_backup in /opt/rkmon-backup.*; do
    [[ $old_backup == "$backup_dir" || ! -d $old_backup ]] && continue
    rm -rf -- "$old_backup"
done
systemctl stop rkmon.service mediamtx.service rkmon-recording.service 2>/dev/null || true
install -d -m 755 /opt/rkmon/lib/recording /opt/rkmon/bin /opt/rkmon/config /opt/rkmon/web /opt/rkmon/models /opt/rkmon/licenses
install -d -m 755 -o elf -g "$(id -gn elf)" /opt/rkmon/logs
# 录像目录独立于部署目录，不参与覆盖和程序备份。
install -d -m 755 -o elf -g "$(id -gn elf)" /userdata/rkmon-video
install -m 644 "$source_dir/lib/recording/"{service,notify}.py /opt/rkmon/lib/recording/
install -m 755 "$source_dir/bin/"{rkmon,mediamtx} /opt/rkmon/bin/
install -m 644 "$source_dir/models/"{yolov8n.rknn,coco_80_labels_list.txt} /opt/rkmon/models/
install -m 644 "$source_dir/licenses/"* /opt/rkmon/licenses/
install -m 644 "$source_dir/config/"{rkmon.ini,recording.ini,mediamtx.yml} /opt/rkmon/config/
cp -R "$source_dir/web/." /opt/rkmon/web/
chmod -R a+rX /opt/rkmon/web
install -m 644 "$source_dir/config/systemd/"{rkmon,mediamtx,rkmon-recording}.service /etc/systemd/system/
install -m 644 "$source_dir/config/nginx-monitor.conf" /etc/nginx/sites-available/rkmon
install -m 644 "$source_dir/config/mosquitto-rkmon.conf" /etc/mosquitto/conf.d/rkmon.conf
ln -sfn /etc/nginx/sites-available/rkmon /etc/nginx/sites-enabled/rkmon
nginx -t
systemctl daemon-reload
systemctl enable nginx.service mosquitto.service mediamtx.service rkmon-recording.service rkmon.service
systemctl restart mosquitto.service mediamtx.service rkmon-recording.service rkmon.service
systemctl reload nginx.service
printf '安装完成；备份：%s\n监控：http://192.168.100.11:9000\n' "$backup_dir"
