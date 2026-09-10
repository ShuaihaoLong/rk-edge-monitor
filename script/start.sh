#!/usr/bin/env bash
# 用途：读取统一 INI 配置，前台启动 rkmon；可从任意目录调用。
# 示例：bash script/start.sh；bash script/start.sh --target host --config /tmp/host.ini
# 参数：--target host|rk3588，默认 rk3588；--config PATH，默认 config/rkmon.ini。
#       --binary PATH 覆盖程序位置；--help 显示说明。相对参数路径以调用目录为基准。
# 环境：无专用环境变量。前提：已编译对应平台程序，配置文件可读、设备权限正确。
# 输出：程序日志按 INI 配置输出，文件相对路径以 INI 所在目录为基准。
# 副作用：打开已启用设备并写日志；不编译、不上传、不后台运行、不修改配置。
set -euo pipefail
usage() { sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; }
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
target=rk3588
config="$project_dir/config/rkmon.ini"
binary=
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --target)
            [[ $# -ge 2 && ( $2 == host || $2 == rk3588 ) ]] || { echo '错误：--target 需要 host 或 rk3588' >&2; exit 2; }
            target=$2; shift 2 ;;
        --config|--binary)
            [[ $# -ge 2 && -n $2 ]] || { echo "错误：$1 需要路径" >&2; exit 2; }
            if [[ $1 == --config ]]; then config=$2; else binary=$2; fi
            shift 2 ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
[[ -n $binary ]] || binary="$project_dir/build/$target/src/app/rkmon"
[[ -f $config && -r $config ]] || { echo "错误：配置文件不可读：$config" >&2; exit 1; }
[[ -f $binary && -x $binary ]] || { echo "错误：程序不存在或不可执行：$binary" >&2; exit 1; }
config=$(realpath -e -- "$config")
binary=$(realpath -e -- "$binary")
# 使用 exec 保持 PID 和退出码，SIGINT/SIGTERM 直接交给应用处理。
exec "$binary" --config "$config"
