#!/usr/bin/env bash
# 用途：在 RK3588 上安装打包好的监控程序、网页和 systemd 服务。
# 示例：sudo bash /tmp/rkmon-deploy/script/install-monitor.sh
# 参数：--help 显示说明；无环境变量。固定运行用户 elf，安装目录 /opt/rkmon。
# 前提：ARM64、Python3/SQLite、ffprobe、Nginx、Rockchip GStreamer/RKNN、SDL2/FreeType/json-c 和中文字体，以 root 运行；
#       包内含 bin/、config/、web/、models/、licenses/ 和 Mosquitto 软件包，无需联网。
# 输出：/opt/rkmon；录像/SQLite 在 /userdata/rkmon-video（部署保留）；备份 /opt/rkmon-backup.*。
# 副作用：更新服务及 9000 端口站点，启用开机启动并重启服务，停用 GDM 并独占 DRM 显示；不修改其他 Nginx 站点。
set -euo pipefail
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
    exit 0
fi
[[ $# == 0 ]] || { echo '错误：不支持的参数' >&2; exit 2; }
[[ $EUID == 0 && $(uname -m) == aarch64 ]] || { echo '错误：请在 ARM64 板卡上以 root 运行' >&2; exit 1; }
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
for file in bin/rkmon-display lib/display/weather.py config/display.json config/systemd/rkmon-display.service config/systemd/rkmon-weather.service licenses/LVGL-LICENSE lib/recording/service.py lib/recording/notify.py config/recording.ini config/systemd/rkmon-recording.service web/recordings.html web/recordings.js web/recordings.css bin/rkmon bin/mediamtx config/rkmon.ini config/mediamtx.yml config/nginx-monitor.conf config/mosquitto-rkmon.conf config/systemd/rkmon.service config/systemd/mediamtx.service web/index.html web/player.js web/vendor/reader.js web/vendor/mqtt.min.js web/detections.js web/device-status.js models/yolov8n.rknn models/coco_80_labels_list.txt licenses/RKNN-Model-Zoo-LICENSE packages/SHA256SUMS script/start.sh script/start_all.sh script/stop.sh script/stop_all.sh; do
    [[ -f $source_dir/$file ]] || { echo "错误：缺少 $file" >&2; exit 1; }
done
id elf >/dev/null
command -v nginx >/dev/null
command -v ffprobe >/dev/null
python3 -c 'import sqlite3, zoneinfo; zoneinfo.ZoneInfo("Asia/Shanghai")'
python3 -m py_compile "$source_dir/lib/recording/"{service,notify}.py "$source_dir/lib/display/weather.py"
python3 - "$source_dir/config/display.json" <<'PY'
import json, pathlib, sys
settings = json.loads(pathlib.Path(sys.argv[1]).read_text())
if not pathlib.Path(settings["font"]).is_file():
    raise SystemExit("缺少中文字体：" + settings["font"])
PY
(
    cd "$source_dir/packages"
    sha256sum --check --strict SHA256SUMS
)
# 已安装且版本不低于离线包时复用，避免应用更新重复操作系统包管理器。
needed_packages=()
for package_file in "$source_dir/packages/"*.deb; do
    package_name=$(dpkg-deb -f "$package_file" Package)
    package_version=$(dpkg-deb -f "$package_file" Version)
    package_arch=$(dpkg-deb -f "$package_file" Architecture)
    installed=$(dpkg-query -W -f='${Status} ${Version}' "$package_name:$package_arch" 2>/dev/null || true)
    if [[ $installed == 'install ok installed '* ]] &&
       dpkg --compare-versions "${installed#install ok installed }" ge "$package_version"; then
        continue
    fi
    needed_packages+=("$package_file")
done
if ((${#needed_packages[@]})); then
    dpkg -i "${needed_packages[@]}"
fi
# 先验证媒体配置和动态库，避免安装不完整的包后停止现有服务。
"$source_dir/bin/mediamtx" --validate-conf "$source_dir/config/mediamtx.yml"
if ldd "$source_dir/bin/rkmon" "$source_dir/bin/rkmon-display" | grep -q 'not found'; then
    echo '错误：rkmon 缺少动态库' >&2
    exit 1
fi
backup_dir=$(mktemp -d /opt/rkmon-backup.XXXXXXXX)
for path in /opt/rkmon /etc/nginx/sites-available/rkmon /etc/nginx/sites-enabled/rkmon /etc/mosquitto/conf.d/rkmon.conf /etc/systemd/system/rkmon.service /etc/systemd/system/mediamtx.service /etc/systemd/system/rkmon-recording.service /etc/systemd/system/rkmon-weather.service /etc/systemd/system/rkmon-display.service /home/elf/.config/systemd/user/rkmon-display.service /home/elf/.config/autostart/rkmon-display.desktop; do
    if [[ -e $path || -L $path ]]; then cp -a --parents "$path" "$backup_dir/"; fi
done
# 新备份已完整生成后，只保留它作为下一次回滚点；备份失败时保留旧备份。
for old_backup in /opt/rkmon-backup.*; do
    [[ $old_backup == "$backup_dir" || ! -d $old_backup ]] && continue
    rm -rf -- "$old_backup"
done
display_uid=$(id -u elf)
user_systemctl() {
    runuser -u elf -- env XDG_RUNTIME_DIR="/run/user/$display_uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$display_uid/bus" systemctl --user "$@"
}
if [[ -S /run/user/$display_uid/bus ]]; then
    user_systemctl disable --now rkmon-display.service || true
fi
systemctl disable --now gdm3.service 2>/dev/null || true
systemctl stop rkmon-weather.service 2>/dev/null || true
systemctl stop rkmon.service mediamtx.service rkmon-recording.service 2>/dev/null || true
install -d -m 755 /opt/rkmon/lib/recording /opt/rkmon/lib/display /opt/rkmon/bin /opt/rkmon/config /opt/rkmon/web /opt/rkmon/models /opt/rkmon/licenses /opt/rkmon/script
install -d -m 755 -o elf -g "$(id -gn elf)" /opt/rkmon/logs
# 录像目录独立于部署目录，不参与覆盖和程序备份。
install -d -m 755 -o elf -g "$(id -gn elf)" /userdata/rkmon-video
install -m 644 "$source_dir/lib/recording/"{service,notify}.py /opt/rkmon/lib/recording/
install -m 755 "$source_dir/bin/"{rkmon,rkmon-display,mediamtx} /opt/rkmon/bin/
install -m 644 "$source_dir/models/"{yolov8n.rknn,coco_80_labels_list.txt} /opt/rkmon/models/
install -m 644 "$source_dir/licenses/"* /opt/rkmon/licenses/
install -m 644 "$source_dir/config/"{rkmon.ini,recording.ini,mediamtx.yml,display.json} /opt/rkmon/config/
cp -R "$source_dir/web/." /opt/rkmon/web/
chmod -R a+rX /opt/rkmon/web
install -m 644 "$source_dir/lib/display/weather.py" /opt/rkmon/lib/display/
install -m 644 "$source_dir/config/systemd/"{rkmon,mediamtx,rkmon-recording,rkmon-weather,rkmon-display}.service /etc/systemd/system/
rm -f /home/elf/.config/systemd/user/rkmon-display.service /home/elf/.config/autostart/rkmon-display.desktop
install -m 644 "$source_dir/config/nginx-monitor.conf" /etc/nginx/sites-available/rkmon
install -m 644 "$source_dir/config/mosquitto-rkmon.conf" /etc/mosquitto/conf.d/rkmon.conf
install -m 755 "$source_dir/script/"{start,start_all,stop,stop_all}.sh /opt/rkmon/script/
ln -sfn /etc/nginx/sites-available/rkmon /etc/nginx/sites-enabled/rkmon
nginx -t
systemctl daemon-reload
systemctl enable nginx.service mosquitto.service mediamtx.service rkmon-recording.service rkmon.service rkmon-display.service
systemctl restart mosquitto.service mediamtx.service rkmon-recording.service rkmon.service
systemctl enable --now rkmon-weather.service
systemctl restart rkmon-display.service
systemctl is-active --quiet rkmon-display.service
systemctl reload nginx.service
printf '安装完成；备份：%s\n监控：http://192.168.100.11:9000\n' "$backup_dir"
