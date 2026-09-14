#!/usr/bin/env bash
# 用途：将 ARM64 主程序、MediaMTX、网页和配置打成离线部署包。
# 示例：bash script/package-monitor.sh
#       bash script/package-monitor.sh --archive /路径/mediamtx_v1.21.0_linux_arm64.tar.gz
# 参数：--archive 指定 MediaMTX 安装包；无环境变量；--help 显示说明。
# 前提：先运行 build.sh --target rk3588 和 prepare-ai-probe.py，准备官方 v1.21.0 ARM64 包及 sha256sum/tar。
# 输出：build/deploy/rkmon-deploy.tar.gz；默认从 .local/downloads/ 读取安装包。
# 副作用：覆盖上次部署包，不下载文件，不连接板卡，不修改源码和板端服务。
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
archive=$project_dir/.local/downloads/mediamtx_v1.21.0_linux_arm64.tar.gz
while (($#)); do
    case "$1" in
        --help|-h) sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
        --archive) [[ $# -ge 2 ]] || { echo '错误：--archive 缺少路径' >&2; exit 2; }; archive=$2; shift 2 ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
[[ -f $project_dir/build/rk3588/src/app/rkmon && -f $archive ]] || { echo '错误：缺少 ARM64 主程序或 MediaMTX 安装包' >&2; exit 1; }
# 与官方 release API 的摘要比对，防止错放架构或不完整的下载包。
expected=a8113b5928ba1a934b81557b61b8a07954b76921a4b567d54c7f086f8b39d9a2
actual=$(sha256sum -- "$archive")
[[ ${actual%% *} == "$expected" ]] || { echo '错误：MediaMTX SHA256 不匹配' >&2; exit 1; }
staging=$(mktemp -d)
trap 'rm -rf -- "$staging"' EXIT
payload=$staging/rkmon-deploy
mkdir -p "$payload/"{bin,config/systemd,script,models,licenses}
# 模型与配套标签随程序一起更新，拒绝未知版本的模型。
model=$project_dir/.local/ai-stage4/yolov8n.rknn
model_digest=$(sha256sum -- "$model")
[[ ${model_digest%% *} == defa25aea179be4da5c5c5826e0be26833b9f818f86b6519620f52f6df3b6a17 ]] || { echo '错误：AI 模型摘要不符' >&2; exit 1; }
cp "$model" "$payload/models/"
cp "$project_dir/.local/ai-stage4/zoo/examples/yolov8/model/coco_80_labels_list.txt" "$payload/models/"
cp "$project_dir/third_party/rknn_yolov8/LICENSE" "$payload/licenses/RKNN-Model-Zoo-LICENSE"
tar -xzf "$archive" -C "$payload/bin" mediamtx
cp "$project_dir/build/rk3588/src/app/rkmon" "$payload/bin/"
cp "$project_dir/config/"{rkmon.ini,mediamtx.yml,nginx-monitor.conf} "$payload/config/"
cp "$project_dir/config/systemd/"{rkmon,mediamtx}.service "$payload/config/systemd/"
cp -R "$project_dir/web" "$payload/"
cp "$script_dir/install-monitor.sh" "$payload/script/"
mkdir -p "$project_dir/build/deploy"
tar -czf "$project_dir/build/deploy/rkmon-deploy.tar.gz" -C "$staging" rkmon-deploy
printf '部署包：%s/build/deploy/rkmon-deploy.tar.gz\n' "$project_dir"
