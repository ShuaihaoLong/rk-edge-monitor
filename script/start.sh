#!/usr/bin/env bash
# 用途：默认重启板端 rkmon systemd 服务；传入开发参数时前台启动 rkmon。
# 示例：sudo bash script/start.sh；bash script/start.sh --target host --config /tmp/host.ini
# 参数：无参数时重启 rkmon.service；--foreground、--target、--config、--binary 使用前台模式。
#       --help 显示说明。相对参数路径以调用目录为基准。
# 环境：服务模式需要 root 和已安装 rkmon.service；前台模式需要已编译程序及可读配置。
# 输出：服务模式输出 systemd 状态；前台模式输出程序日志。
# 副作用：服务模式重启 rkmon；前台模式打开设备并写日志，不修改配置。
set -euo pipefail
usage() { sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; }
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
target=rk3588
config="$project_dir/config/rkmon.ini"
binary=
foreground=false
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --foreground) foreground=true; shift ;;
        --target)
            [[ $# -ge 2 && ( $2 == host || $2 == rk3588 ) ]] || { echo '错误：--target 需要 host 或 rk3588' >&2; exit 2; }
            target=$2; foreground=true; shift 2 ;;
        --config|--binary)
            [[ $# -ge 2 && -n $2 ]] || { echo "错误：$1 需要路径" >&2; exit 2; }
            if [[ $1 == --config ]]; then config=$2; else binary=$2; fi
            foreground=true
            shift 2 ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
if ! "$foreground"; then
    [[ $EUID == 0 ]] || { echo '错误：服务模式请以 root 运行；开发前台模式请使用 --foreground' >&2; exit 1; }
    systemctl restart rkmon.service
    systemctl is-active --quiet rkmon.service
    echo 'rkmon 已重启。'
    exit 0
fi
[[ -n $binary ]] || binary="$project_dir/build/$target/src/app/rkmon"
[[ -f $config && -r $config ]] || { echo "错误：配置文件不可读：$config" >&2; exit 1; }
[[ -f $binary && -x $binary ]] || { echo "错误：程序不存在或不可执行：$binary" >&2; exit 1; }
config=$(realpath -e -- "$config")
binary=$(realpath -e -- "$binary")
# 使用 exec 保持 PID 和退出码，SIGINT/SIGTERM 直接交给应用处理。
exec "$binary" --config "$config"
