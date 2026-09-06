#!/usr/bin/env bash
# 用途：构建 rkmon 主工程和中间层；使用 CMake + Unix Makefiles。
# 使用：bash script/build.sh [--target host|rk3588] [--jobs 正整数] [--test]
#       bash script/build.sh --help
# 默认：target=rk3588；build/<target> 存放产物，RK3588_SYSROOT 可覆盖板端快照路径。
# 前提：主机编译需要 g++；交叉编译需要 ARM64 GCC/G++ 和已准备好的 sysroot。
# 行为：--test 仅在 host 模式运行 CTest，禁止在 WSL 直接运行 ARM64 程序。
# 输出：build/<target>/src/app/rkmon、compile_commands.json 和测试程序。
# 说明：从任意目录调用均可；不下载依赖、不部署到板卡、不修改第三方源码。
set -euo pipefail
usage() { sed -n '2,9s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; }
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
target=rk3588
jobs=$(nproc)
run_tests=false
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --target)
            [[ $# -ge 2 && ( $2 == host || $2 == rk3588 ) ]] || { echo '错误：--target 需要 host 或 rk3588' >&2; exit 2; }
            target=$2; shift 2 ;;
        --jobs)
            [[ $# -ge 2 && $2 =~ ^[1-9][0-9]*$ ]] || { echo '错误：--jobs 需要正整数' >&2; exit 2; }
            jobs=$2; shift 2 ;;
        --test) run_tests=true; shift ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
if "$run_tests" && [[ $target != host ]]; then
    echo '错误：--test 仅支持 --target host；ARM64 测试需要部署到板端运行' >&2
    exit 2
fi
args=(-G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRKMON_BUILD_TESTS=ON)
if [[ $target == rk3588 ]]; then
    export RK3588_SYSROOT
    RK3588_SYSROOT=$(realpath -e -- "${RK3588_SYSROOT:-$project_dir/.local/sysroots/rk3588}")
    args+=(-DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/toolchains/rk3588-linux.cmake")
fi
cmake -S "$project_dir" -B "$project_dir/build/$target" "${args[@]}"
make -C "$project_dir/build/$target" -j"$jobs"
if "$run_tests"; then ctest --test-dir "$project_dir/build/$target" --output-on-failure; fi
