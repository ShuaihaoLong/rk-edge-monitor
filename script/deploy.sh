#!/usr/bin/env bash
# 用途：从 PC 一键构建、打包、上传并部署/更新板端监控服务。
# 示例：bash script/deploy.sh；bash script/deploy.sh --host elf@192.168.100.11 --skip-build
# 参数：--host SSH 目标（默认 elf@192.168.100.11）；--jobs 构建并行数（默认 4）；
#       --archive MediaMTX 包路径；--skip-build 复用已有 ARM64 程序；--dry-run 仅显示计划；--help。
# 前提：PC 有 Bash、SSH 密钥、交叉编译工具及已准备的媒体/AI/MQTT 包；板端有 Nginx、MPP/GStreamer、RKNN、sudo。
# 输出：build/deploy/rkmon-deploy.tar.gz；板端 /opt/rkmon 和 /opt/rkmon-backup.*；无环境变量。
# 副作用：短暂停流，覆盖程序、网页和项目配置，启用/重启服务；sudo 可能在终端询问密码，不保存密码。
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
host=elf@192.168.100.11
jobs=4
archive=$project_dir/.local/downloads/mediamtx_v1.21.0_linux_arm64.tar.gz
skip_build=false
dry_run=false
while (($#)); do
    case "$1" in
        --help|-h) sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
        --host|--jobs|--archive)
            [[ $# -ge 2 && -n $2 ]] || { echo "错误：$1 缺少参数" >&2; exit 2; }
            case "$1" in --host) host=$2;; --jobs) jobs=$2;; --archive) archive=$2;; esac
            shift 2 ;;
        --skip-build) skip_build=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
[[ $host =~ ^[a-zA-Z0-9_][a-zA-Z0-9_.@:-]*$ ]] || { echo '错误：SSH 目标格式无效' >&2; exit 2; }
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo '错误：--jobs 必须为正整数' >&2; exit 2; }
if "$dry_run"; then
    printf '目标：%s\n跳过构建：%s\n并行数：%s\n安装包：%s\n' "$host" "$skip_build" "$jobs" "$archive"
    echo '流程：检查 SSH → 构建 → 校验并打包 → 上传独立临时目录 → sudo 安装并备份 → 检查服务和 HTTP → 清理临时目录'
    echo '更新会覆盖 config/ 中的项目配置；--host 只改变 SSH 目标，运行用户和 WebRTC 地址仍以配置文件为准。'
    exit 0
fi
# 在耗时构建之前检查目标；不把密码写入命令、文件或日志。
ssh_options=(-o BatchMode=yes -o ConnectTimeout=10)
ssh "${ssh_options[@]}" "$host" 'test "$(uname -m)" = aarch64 && command -v sudo >/dev/null && command -v nginx >/dev/null && command -v curl >/dev/null'
if ! "$skip_build"; then bash "$script_dir/build.sh" --target rk3588 --jobs "$jobs"; fi
bash "$script_dir/package-monitor.sh" --archive "$archive"
# 每次使用独立目录，避免失败重试混入旧文件。只清理本次创建的目录。
remote_dir=$(ssh "${ssh_options[@]}" "$host" 'mktemp -d /tmp/rkmon-upload.XXXXXXXX')
[[ $remote_dir =~ ^/tmp/rkmon-upload\.[a-zA-Z0-9]+$ ]] || { echo '错误：远程临时目录无效' >&2; exit 1; }
cleanup() {
    ssh "${ssh_options[@]}" "$host" "rm -rf -- '$remote_dir'" || echo "提示：请手动清理 $remote_dir" >&2
}
trap cleanup EXIT
ssh "${ssh_options[@]}" "$host" "tar -xzf - -C '$remote_dir'" < "$project_dir/build/deploy/rkmon-deploy.tar.gz"
# 分配终端交给 sudo 交互；已有免密 sudo 时可直接完成。
ssh -tt "${ssh_options[@]}" "$host" "sudo bash '$remote_dir/rkmon-deploy/script/install-monitor.sh'"
ssh "${ssh_options[@]}" "$host" 'sleep 3; systemctl is-active --quiet rkmon && systemctl is-active --quiet mediamtx && systemctl is-active --quiet mosquitto && systemctl is-active --quiet nginx && curl --noproxy "*" --fail --silent --output /dev/null --max-time 5 http://127.0.0.1:9000/'
echo '部署完成：服务运行且监控页面返回成功。请在浏览器确认实际画面。'
