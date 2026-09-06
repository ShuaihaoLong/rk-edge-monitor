#!/usr/bin/env bash
# 用途：通过 SSH 获取 RK3588 视频开发 sysroot，成功后自动修复软链接。
# 使用：bash script/sync-sysroot.sh [--host 用户@地址] [--dest 新目录] [--dry-run]
#       bash script/sync-sysroot.sh --help
# 默认：host=elf@192.168.100.11；dest=项目/.local/sysroots/rk3588。
# 前提：本机有 rsync、ssh、python3、realpath；板端有 rsync 和 ARM64 开发文件。
# 空间：本地至少剩余 8 GiB；大型板端安装可能需要更多，可用 SYSROOT_MIN_GIB 调高。
# 行为：不修改板端、不保存密码；SSH 密码交互输入，也可使用已配置的 SSH 密钥。
#       正式同步只允许目标目录不存在，先下载到相邻暂存目录，检查成功再发布。
#       已有快照时用 --dest 指定新目录，避免混入旧库；失败保留暂存文件供排查。
#       --dry-run 只试运行 rsync，不下载文件、不修复链接，也不创建目录。
# 输出：目标 sysroot 和旁边的 <目录名>-link-report.json；缺少无关链接会记录。
set -euo pipefail

usage() {
    sed -n '2,12s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
remote=elf@192.168.100.11
destination="$project_dir/.local/sysroots/rk3588"
dry_run=false
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --host|--dest)
            [[ $# -ge 2 && -n $2 && $2 != --* ]] || { echo "错误：$1 缺少值" >&2; exit 2; }
            if [[ $1 == --host ]]; then remote=$2; else destination=$2; fi
            shift 2 ;;
        --dry-run) dry_run=true; shift ;;
        *) echo "错误：未知参数 $1" >&2; usage >&2; exit 2 ;;
    esac
done
# 限定为 SSH 用户@主机或 SSH 主机别名，避免把远端参数解释为 rsync 选项。
[[ $remote =~ ^[a-zA-Z0-9_][a-zA-Z0-9_.@-]*$ ]] || { echo '错误：无效的 SSH 主机格式' >&2; exit 2; }
for tool in rsync ssh python3 realpath df; do
    command -v "$tool" >/dev/null || { echo "错误：缺少工具 $tool" >&2; exit 1; }
done
destination=$(realpath -m -- "$destination")
rsync_args=(-aR --no-owner --no-group --stats
    --exclude=/usr/lib/ssl/private
    --exclude=/usr/lib/cups/backend/cups-brf
    --exclude=/usr/lib/cups/backend/implicitclass
    -e 'ssh -o ConnectTimeout=10')
# 多源路径来自同一个 SSH 连接，保留 /usr 层级和原始软链接，不使用 -L。
sources=("$remote:/./usr/include" ':/./usr/lib' ':/./usr/share/pkgconfig')
if "$dry_run"; then
    rsync "${rsync_args[@]}" --dry-run "${sources[@]}" "$destination/"
    exit 0
fi
[[ ! -e "$destination" && ! -L "$destination" ]] || {
    echo "错误：目标已存在：$destination；请用 --dest 指定新快照目录" >&2
    exit 1
}
minimum_gib=${SYSROOT_MIN_GIB:-8}
[[ $minimum_gib =~ ^[1-9][0-9]{0,3}$ ]] || { echo '错误：SYSROOT_MIN_GIB 必须为 1 至 9999' >&2; exit 2; }
# 沿父目录向上找到现存目录，检查目标所在文件系统，尚不创建文件。
space_dir=$(dirname -- "$destination")
while [[ ! -d "$space_dir" ]]; do space_dir=$(dirname -- "$space_dir"); done
available=$(df -PB1 -- "$space_dir" | awk 'END {print $4}')
((available >= minimum_gib * 1024 * 1024 * 1024)) || { echo "错误：剩余空间不足 $minimum_gib GiB" >&2; exit 1; }
parent_dir=$(dirname -- "$destination")
mkdir -p -- "$parent_dir"
staging=$(mktemp -d "$parent_dir/.sysroot-download.XXXXXX")
trap 'echo "同步未完成；暂存目录：$staging" >&2' ERR
# 使用暂存目录，只有传输和关键依赖检查全部成功才形成可用快照。
rsync "${rsync_args[@]}" "${sources[@]}" "$staging/"
python3 "$script_dir/fix-sysroot-links.py" "$staging"
# -T 避免目标并发出现时将快照意外嵌套到另一个目录下。
[[ ! -e "$destination" && ! -L "$destination" ]]
mv -T -- "$staging" "$destination"
mv -- "$parent_dir/$(basename -- "$staging")-link-report.json" "$destination-link-report.json"
trap - ERR
printf '\n同步完成：%s\n使用前设置：export RK3588_SYSROOT=%q\n' "$destination" "$destination"
