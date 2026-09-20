#!/usr/bin/env bash
# 用途：准备本地屏幕使用的固定版本 LVGL 源码，构建仍由 build.sh 执行。
# 示例：bash script/prepare-display-deps.sh；bash script/prepare-display-deps.sh --offline
# 参数：--offline 只校验已有依赖；--help 显示说明；无专用环境变量。
# 前提：Git、网络（离线模式除外）；ARM64 sysroot 需有 SDL2、FreeType、json-c、GStreamer 开发文件。
# 输出：项目 .local/lvgl/source，固定 v9.4.0 提交；从任意工作目录调用均可。
# 副作用：在线模式下载第三方源码，不安装系统包，不修改板端和正式服务。
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
offline=false
while (($#)); do
    case "$1" in
        --offline) offline=true; shift ;;
        --help|-h) sed -n '2,7s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
source_dir=$project_dir/.local/lvgl/source
revision=c016f72d4c125098287be5e83c0f1abed4706ee5
if [[ ! -d $source_dir/.git ]]; then
    "$offline" && { echo '错误：离线模式缺少 LVGL' >&2; exit 1; }
    mkdir -p "$(dirname -- "$source_dir")"
    git clone --depth 1 --branch v9.4.0 --filter=blob:none --sparse https://github.com/lvgl/lvgl.git "$source_dir"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || { echo '错误：LVGL 提交不匹配' >&2; exit 1; }
if ! "$offline"; then
    git -C "$source_dir" sparse-checkout init --cone
    git -C "$source_dir" sparse-checkout set src env_support
fi
[[ -f $source_dir/lvgl.h && -f $source_dir/src/core/lv_obj.c && -f $source_dir/env_support/cmake/os_desktop.cmake ]] || {
    echo '错误：LVGL 检出不完整，请在线运行准备脚本' >&2; exit 1;
}
[[ -z $(git -C "$source_dir" status --porcelain --untracked-files=no) ]] || { echo '错误：LVGL 源码有本地修改' >&2; exit 1; }
echo "LVGL 已准备：$revision"
