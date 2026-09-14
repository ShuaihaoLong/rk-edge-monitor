#!/usr/bin/env python3
# 用途：准备独立 YOLOv8 板端探针所需的固定版本源码、公开样图与预转换模型。
# 示例：python3 script/prepare-ai-probe.py；python3 script/prepare-ai-probe.py --offline
# 参数：--offline 仅校验本地文件；--help 显示说明；无环境变量。
# 前提：Python3、curl；下载使用 HTTPS 直连 GitHub API 与 Mixtile，不需要 pip。
# 输出：项目 .local/ai-stage4/，模型及依赖不加入 Git；从任意工作目录均可运行。
# 副作用：联网下载缺失/不匹配的文件；不安装 Python 包、不修改板端服务和系统库。
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess

COMMIT = "c2b7d00714b4e5d21266ab3003f3ca687ba0d57b"  # Model Zoo v2.1.0
MODEL_URL = "https://downloads.mixtile.com/doc-files/yolov8/rk3588/yolov8n.rknn"
# 固定本次实测文件的摘要，用于重复验证；并非厂商发布的签名。
MODEL_SHA256 = "defa25aea179be4da5c5c5826e0be26833b9f818f86b6519620f52f6df3b6a17"
FILES = [
    "LICENSE", "3rdparty/rknpu2/include/rknn_api.h",
    "3rdparty/stb_image/LICENSE.txt", "3rdparty/stb_image/stb_image.h",
    "3rdparty/stb_image/stb_image_write.h", "examples/yolov8/cpp/postprocess.cc",
    "examples/yolov8/cpp/postprocess.h", "examples/yolov8/cpp/yolov8.h",
    "examples/yolov8/model/bus.jpg", "examples/yolov8/model/coco_80_labels_list.txt",
    "utils/common.h", "utils/image_utils.h",
]

def download(url):
    return subprocess.check_output([
        "curl", "--noproxy", "*", "-fsSL", "--connect-timeout", "10",
        "--max-time", "90", "--retry", "1", url,
    ])

def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_bytes(data)
    temporary.replace(path)

def blob_sha(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()

def main():
    parser = argparse.ArgumentParser(description="准备 RK3588 YOLOv8 能力探针依赖，不安装系统软件。")
    parser.add_argument("--offline", action="store_true", help="只校验已有文件，不联网")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent / ".local/ai-stage4"
    tree_path = root / "zoo-tree.json"
    if not tree_path.exists():
        if args.offline:
            raise RuntimeError("缺少 zoo-tree.json，请先联网准备")
        save(tree_path, download(f"https://api.github.com/repos/airockchip/rknn_model_zoo/git/trees/{COMMIT}?recursive=1"))
    tree = json.loads(tree_path.read_text())
    if tree.get("sha") != COMMIT or tree.get("truncated"):
        raise RuntimeError("源码清单不是指定的完整提交")
    entries = {entry["path"]: entry for entry in tree["tree"]}
    for name in FILES:
        path = root / "zoo" / name
        digest = entries[name]["sha"]
        if path.exists() and blob_sha(path.read_bytes()) == digest:
            continue
        if args.offline:
            raise RuntimeError(f"源码缺失或摘要不符：{name}")
        # URL 由固定仓库与 blob 摘要构造，不执行下载内容。
        response = json.loads(download(f"https://api.github.com/repos/airockchip/rknn_model_zoo/git/blobs/{digest}"))
        data = base64.b64decode(response["content"])
        if blob_sha(data) != digest:
            raise RuntimeError(f"源码校验失败：{name}")
        save(path, data)
    model = root / "yolov8n.rknn"
    if not model.exists() or hashlib.sha256(model.read_bytes()).hexdigest() != MODEL_SHA256:
        if args.offline:
            raise RuntimeError("模型缺失或摘要不符")
        data = download(MODEL_URL)
        if hashlib.sha256(data).hexdigest() != MODEL_SHA256:
            raise RuntimeError("模型版本已变化或下载不完整，请核对来源后更新固定摘要")
        save(model, data)
    print(f"依赖校验通过：{root}")

if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"准备失败：{error}")
