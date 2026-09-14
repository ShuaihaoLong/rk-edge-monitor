# RK3588 YOLOv8 单图能力验证

本探针独立于正式监控程序，通过 RKNN C API 验证预转换模型、运行库和 NPU 驱动可用性。
不修改 `/opt/rkmon`，不停止相机/编码服务，不安装或覆盖系统 `librknnrt.so`。

## 模型与依赖

- 预转换模型：[Mixtile 提供的 RK3588 YOLOv8n](https://downloads.mixtile.com/doc-files/yolov8/rk3588/yolov8n.rknn)。[厂商使用说明](https://www.mixtile.com/docs/running-yolov8-on-mixtile-blade-3/)。
- 模型文件大小：4,313,686 字节（以实际文件和下面的摘要为准）。
- SHA256：`defa25aea179be4da5c5c5826e0be26833b9f818f86b6519620f52f6df3b6a17`。这是本次实测文件的固定摘要，不是厂商签名。
- 模型元数据：RK3588、INT8、输入 NHWC 1×640×640×3，9 个输出；转换工具 `2.0.0b0+9bab5682`。
- API 头文件、后处理及样图：[Rockchip Model Zoo v2.1.0](https://github.com/airockchip/rknn_model_zoo/tree/c2b7d00714b4e5d21266ab3003f3ca687ba0d57b)，提交 `c2b7d00714b4e5d21266ab3003f3ca687ba0d57b`。
- 下载源码保留其许可证；本探针不修改官方后处理文件。

此模型与后处理必须配套。程序启动会校验输入、输出数量/布局/量化类型，拒绝不匹配的模型。
仅支持随附 640×640 `bus.jpg`，不做缩放/补边；正式视频接入时另行实现 NV12 预处理。

## WSL 构建与板端运行

在仓库根目录执行：

```bash
# 下载并校验固定版本依赖；本次已完成，可以加 --offline 只校验已有文件。
python3 script/prepare-ai-probe.py
RK3588_SYSROOT="$PWD/.local/sysroots/rk3588" cmake \
  -S examples/rknn-smoke -B build/rknn-smoke -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/rk3588-linux.cmake" \
  -DCMAKE_BUILD_TYPE=Release
make -C build/rknn-smoke -j4
ssh elf@192.168.100.11 'mkdir -p /tmp/rkmon-ai-stage4/model'
scp build/rknn-smoke/rkmon_rknn_smoke elf@192.168.100.11:/tmp/rkmon-ai-stage4/
scp .local/ai-stage4/yolov8n.rknn \
  .local/ai-stage4/zoo/examples/yolov8/model/bus.jpg \
  .local/ai-stage4/zoo/examples/yolov8/model/coco_80_labels_list.txt \
  elf@192.168.100.11:/tmp/rkmon-ai-stage4/model/
ssh elf@192.168.100.11 \
  'cd /tmp/rkmon-ai-stage4 && timeout 60 ./rkmon_rknn_smoke model/yolov8n.rknn model/bus.jpg'
```

输出 `result.png` 与终端统计；返回 0 且包含 `PASS` 才代表通过。模型无法读取、布局不匹配、API 失败或样图检测结果异常均返回非零。
只在这个探针中要求每次都检出 person 和 bus；真实监控画面没有目标属于正常情况。

## 本次实测

日期：2026-09-14；Runtime 2.1.0；NPU 驱动 0.9.6；监控服务保持运行。
五次预热后统计三十次，共三十五次成功，逐次检查 person/bus 和框坐标范围。

| 指标 | 平均 | P50 | P95 |
| --- | --- | --- | --- |
| rknn_run 调用耗时 | 15.86 ms | 15.58 ms | 17.74 ms |
| 输入提交到后处理完成 | 18.77 ms | 18.64 ms | 20.64 ms |

后者不含 JPEG 读取、视频缩放/颜色转换、输出资源释放、绘图和编码传输；不能直接当成实时视频 FPS。
结果为 4 个 person 和 1 个 bus，主要目标置信度约 0.85–0.87；这是能力验证，不是精度数据集评测。
结果与日志位于 `.local/ai-stage4/results/`，板端副本在 `/tmp/rkmon-ai-stage4/`。

## 后续在 WSL 转换模型

Rockchip 官方确认 [WSL2 Ubuntu 22.04 可使用 Toolkit2](https://github.com/airockchip/rknn-toolkit2/blob/master/doc/Using%20RKNN-ToolKit2%20in%20WSL.md)。
当前 PC 是 x86_64 Ubuntu 22.04.5、Python 3.10.12，尚未安装 pip、venv 配套包和 Toolkit2。
本次选择已转换的模型完成验证，没有安装完整转换环境。

后续转换应在独立 Python 虚拟环境中安装与板端兼容的 Toolkit2，使用配套 YOLOv8 ONNX，
指定 `target_platform='rk3588'`。先建立非量化结果基准，再使用代表性样图做 INT8 校准。
转换完成后固定模型摘要、Toolkit2 版本和前后处理契约，再接入正式 `src/ai` 与部署链路。
