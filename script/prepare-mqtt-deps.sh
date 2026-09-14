#!/usr/bin/env bash
# 用途：下载并校验 Ubuntu 22.04 ARM64 的 Mosquitto 2.0.11 离线部署包。
# 示例：bash script/prepare-mqtt-deps.sh；bash script/prepare-mqtt-deps.sh --offline
# 参数：--offline 仅校验现有文件；--help 显示说明；无环境变量。
# 前提：PC 有 curl、sha256sum；下载源为 USTC 的 Ubuntu ports 镜像。
# 输出：项目 .local/downloads/mosquitto-arm64/；从任意工作目录均可运行。
# 副作用：联网下载缺失或摘要不符的软件包；不安装软件、不连接或修改板卡。
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
destination=$project_dir/.local/downloads/mosquitto-arm64
offline=false
case "${1:-}" in
    --help|-h) sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
    --offline) offline=true ;;
    "") ;;
    *) echo "错误：未知参数 $1" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo '错误：参数过多' >&2; exit 2; }
mkdir -p "$destination"
base=https://mirrors.ustc.edu.cn/ubuntu-ports
packages=(
    'pool/universe/d/dlt-daemon/libdlt2_2.18.6-2_arm64.deb|d4fe2f698df9706c89efb027c025b89c3aa9d4153decd1a08f8e9e2e40da4d93'
    'pool/universe/libe/libev/libev4_4.33-1_arm64.deb|726d2812a46778e3f9129d56e139e8d91ec36a87315cf9c1d62496fa3e244efe'
    'pool/universe/m/mosquitto/libmosquitto1_2.0.11-1ubuntu1.2_arm64.deb|658c0b18ae31124fe5c263fab6c7006e2850e572b4743369fd2e5449461c3b29'
    'pool/universe/libw/libwebsockets/libwebsockets16_4.0.20-2ubuntu1.1_arm64.deb|5c64b21493b4a6a1279b6aa169332f4cbde7683bb7b0e09399dc7ae90e17e98c'
    'pool/universe/m/mosquitto/mosquitto_2.0.11-1ubuntu1.2_arm64.deb|1d29939ce2931fb80b0defcc4d03646622017d197b7c1c243329ee85a17cdf08'
)
for item in "${packages[@]}"; do
    path=${item%%|*}
    expected=${item##*|}
    name=${path##*/}
    actual=''
    [[ -f $destination/$name ]] && actual=$(sha256sum -- "$destination/$name")
    if [[ ${actual%% *} != "$expected" ]]; then
        "$offline" && { echo "错误：缺少或校验失败：$name" >&2; exit 1; }
        temporary=$destination/$name.part
        curl --noproxy '*' -fL --connect-timeout 10 --max-time 90 --retry 1 \
            -o "$temporary" "$base/$path"
        actual=$(sha256sum -- "$temporary")
        [[ ${actual%% *} == "$expected" ]] || { echo "错误：摘要不符：$name" >&2; exit 1; }
        mv -- "$temporary" "$destination/$name"
    fi
done
echo "Mosquitto 离线包校验通过：$destination"
