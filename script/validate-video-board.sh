#!/usr/bin/env bash
# 用途：在 RK3588 板端检查 GStreamer/MPP 依赖，运行有限帧数的解码、编码链路。
# 使用：bash script/validate-video-board.sh [--device /dev/videoX] [--frames 150]
#       bash script/validate-video-board.sh --output /tmp/video-check --timeout 30
# 参数：--device 默认使用工程 SYD 相机的稳定设备路径；--frames 默认 150。
#       --timeout 为每条管线最长运行秒数，默认 30；--output 必须是尚不存在的目录。
# 环境：沿用板端 GST_PLUGIN_PATH 等 GStreamer 环境变量，不安装或替换软件。
# 前提：板端 Bash、timeout、gst-inspect-1.0、gst-launch-1.0；当前用户可访问相机/MPP。
# 输出：默认在 /tmp 下创建 rkmon-video-check.*，保存 inventory.log、插件说明、管线日志和 summary.tsv。
# 副作用：短暂占用相机和编解码硬件，写入诊断日志，不保存图像、不停止其他服务。
# 返回：全部检查通过为 0，存在失败/缺少插件为 1，参数或运行前提错误为 2。
set -euo pipefail

usage() {
    sed -n '2,11s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}
# 脚本也可单独复制到板端运行，不依赖调用者当前目录或仓库中的其他文件。
device=/dev/v4l/by-id/usb-SYD_USB_Camera_200901010001-video-index0
frames=150
limit=30
output=
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --device|--frames|--timeout|--output)
            [[ $# -ge 2 && -n $2 ]] || { echo "错误：$1 缺少参数" >&2; exit 2; }
            case "$1" in
                --device) device=$2 ;;
                --frames) frames=$2 ;;
                --timeout) limit=$2 ;;
                --output) output=$2 ;;
            esac
            shift 2 ;;
        *) echo "错误：未知参数 $1" >&2; exit 2 ;;
    esac
done
[[ $frames =~ ^[1-9][0-9]*$ && $limit =~ ^[1-9][0-9]*$ ]] || {
    echo '错误：帧数和超时必须是正整数' >&2; exit 2;
}
for tool in timeout gst-inspect-1.0 gst-launch-1.0; do
    command -v "$tool" >/dev/null || { echo "错误：缺少 $tool" >&2; exit 2; }
done
if [[ -n $output ]]; then
    mkdir -- "$output"
else
    output=$(mktemp -d /tmp/rkmon-video-check.XXXXXX)
fi
output=$(cd -- "$output" && pwd)
printf '日志目录：%s\n' "$output"
printf 'check\tstatus\tdetail\n' > "$output/summary.tsv"
failed=0
record() {
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" | tee -a "$output/summary.tsv"
    if [[ $2 != PASS ]]; then failed=1; fi
}

# 文件、设备节点和插件存在只说明具备候选依赖，实际可用性由后面的管线检查。
{
    date -Is
    uname -a
    cat /etc/os-release
    id
    gst-inspect-1.0 --version
    for node in /dev/mpp_service /dev/rga /dev/dri/renderD* /dev/video*; do
        if [[ -e $node ]]; then ls -l -- "$node"; fi
    done
    if command -v pkg-config >/dev/null; then
        for package in gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtsp-server-1.0; do
            printf '\n%s: ' "$package"
            pkg-config --modversion "$package" || true
        done
    fi
    if command -v dpkg-query >/dev/null; then
        dpkg-query -W '*gstreamer*' '*rockchip*' '*mpp*' '*rga*' 2>&1 || true
    fi
    if command -v v4l2-ctl >/dev/null; then
        v4l2-ctl --device "$device" --all --list-formats-ext || true
    fi
} > "$output/inventory.log" 2>&1

declare -A available
for element in appsrc appsink videotestsrc fakesink v4l2src jpegparse jpegdec jpegenc mppjpegdec mpph264enc h264parse avdec_h264 openh264dec; do
    if timeout -k 2s "${limit}s" gst-inspect-1.0 "$element" > "$output/inspect-$element.log" 2>&1; then
        available[$element]=1
    else
        available[$element]=0
    fi
done
for element in appsrc appsink mppjpegdec mpph264enc; do
    if [[ ${available[$element]} == 1 ]]; then
        record "plugin-$element" PASS "inspect-$element.log；仅确认可加载"
    else
        record "plugin-$element" FAIL "inspect-$element.log"
    fi
done

run_pipeline() {
    local name=$1
    shift
    local result=0
    # 每条管线有限输入并受 timeout 保护，退出失败后仍执行后续独立检查。
    printf '%q ' gst-launch-1.0 -e -v "$@" > "$output/$name.log"
    printf '\n' >> "$output/$name.log"
    timeout -k 3s "${limit}s" gst-launch-1.0 -e -v "$@" >> "$output/$name.log" 2>&1 || result=$?
    # gst-launch 可能在未输出帧时也以 0 退出，必须检查末端实际接收记录。
    local received
    received=$(grep -c 'GstFakeSink:verify: last-message = chain' "$output/$name.log" || true)
    if [[ $result == 0 && $received -gt 0 ]]; then
        record "$name" PASS "$name.log；末端 buffer 记录=$received，未验证视觉质量"
    else
        record "$name" FAIL "$name.log；exit=$result，末端 buffer 记录=$received"
    fi
}

run_pipeline gst-basic videotestsrc num-buffers=1 '!' \
    'video/x-raw,format=RGB,width=320,height=240' '!' fakesink name=verify silent=false sync=false

# 使用合成画面独立验证编码器，避免相机或 JPEG 解码失败掩盖编码能力。
if [[ ${available[mpph264enc]} == 1 && ${available[h264parse]} == 1 ]]; then
    run_pipeline hw-h264-encode videotestsrc num-buffers="$frames" '!' \
        'video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1' '!' \
        mpph264enc '!' h264parse '!' fakesink name=verify silent=false sync=false
    decoder=
    if [[ ${available[avdec_h264]} == 1 ]]; then
        decoder=avdec_h264
    elif [[ ${available[openh264dec]} == 1 ]]; then
        decoder=openh264dec
    fi
    if [[ -n $decoder ]]; then
        run_pipeline h264-roundtrip videotestsrc num-buffers=30 '!' \
            'video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1' '!' \
            mpph264enc '!' h264parse '!' "$decoder" '!' fakesink name=verify silent=false sync=false
    else
        record h264-roundtrip SKIP '缺少 avdec_h264/openh264dec，未验证编码输出能否解码'
    fi
else
    record hw-h264-encode SKIP '缺少 mpph264enc 或 h264parse'
fi

if [[ ! -c $device ]]; then
    record camera-decode SKIP "相机节点不存在：$device"
else
    # 一次只启动一条相机管线，不结束占用设备的其他进程。
    camera=(v4l2src "device=$device" "num-buffers=$frames" '!' \
        'image/jpeg,width=1920,height=1080,framerate=30/1')
    if [[ ${available[jpegparse]} == 1 ]]; then
        camera+=('!' jpegparse)
    fi
    if [[ ${available[jpegdec]} == 1 ]]; then
        run_pipeline camera-sw-jpeg "${camera[@]}" '!' jpegdec '!' fakesink name=verify silent=false sync=false
    else
        record camera-sw-jpeg SKIP '缺少 jpegdec'
    fi
    if [[ ${available[mppjpegdec]} == 1 ]]; then
        run_pipeline camera-hw-jpeg "${camera[@]}" '!' mppjpegdec '!' fakesink name=verify silent=false sync=false
        # 厂商插件需要显式 format=NV12；只接 capsfilter 不能保证触发转换。
        run_pipeline camera-hw-jpeg-nv12 "${camera[@]}" '!' mppjpegdec format=NV12 '!' \
            'video/x-raw,format=NV12' '!' fakesink name=verify silent=false sync=false
    else
        record camera-hw-jpeg SKIP '缺少 mppjpegdec'
    fi
fi
printf '\n检查完成，详细结果：%s/summary.tsv\n' "$output"
exit "$failed"
