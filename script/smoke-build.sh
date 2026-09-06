#!/usr/bin/env bash
# 用途：在 WSL 中用 ARM64 工具链和 Unix Makefiles 构建 GStreamer 验证程序。
# 使用：bash script/smoke-build.sh [--jobs 正整数]
#       bash script/smoke-build.sh --help
# 环境：RK3588_SYSROOT 可指定 sysroot，默认项目/.local/sysroots/rk3588。
#       SMOKE_BUILD_DIR 可指定构建目录，默认项目/build/rk3588-gst-smoke-make。
# 前提：已安装 CMake、Make、pkg-config、ARM64 GCC/G++，并准备好 sysroot。
# 输出：构建目录/gst-smoke 和 compile_commands.json（供 clangd 使用）；不部署或运行 ARM64 产物。
# 说明：从任何工作目录调用均可；相对环境变量路径以调用时的目录为基准。
set -euo pipefail

usage() {
    sed -n '2,9s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
jobs=$(nproc)
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --jobs)
            [[ $# -ge 2 && $2 =~ ^[1-9][0-9]*$ ]] || { echo '错误：--jobs 需要正整数' >&2; exit 2; }
            jobs=$2; shift 2 ;;
        *) echo "错误：未知参数 $1" >&2; usage >&2; exit 2 ;;
    esac
done
for tool in cmake make pkg-config aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ realpath; do
    command -v "$tool" >/dev/null || { echo "错误：缺少工具 $tool" >&2; exit 1; }
done
export RK3588_SYSROOT
RK3588_SYSROOT=$(realpath -e -- "${RK3588_SYSROOT:-$project_dir/.local/sysroots/rk3588}")
build_dir=$(realpath -m -- "${SMOKE_BUILD_DIR:-$project_dir/build/rk3588-gst-smoke-make}")
# 提前识别旧生成器缓存，避免用户误删已有构建产物来解决冲突。
if [[ -f "$build_dir/CMakeCache.txt" ]] &&
   ! grep -qx 'CMAKE_GENERATOR:INTERNAL=Unix Makefiles' "$build_dir/CMakeCache.txt"; then
    echo '错误：已有构建目录使用其他生成器，请通过 SMOKE_BUILD_DIR 指定新目录' >&2
    exit 1
fi
cmake -S "$project_dir/examples/gst-smoke" -B "$build_dir" \
    -G 'Unix Makefiles' \
    -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/toolchains/rk3588-linux.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -C "$build_dir" -j"$jobs"
printf '\n构建完成：%s/gst-smoke\n' "$build_dir"
