# AI 模块

## 职责与入口

AI 负责从视频分支获取 NV12 帧、预处理、RKNN 推理、YOLOv8 后处理和检测结果输出。代码位于 `src/ai/`，公共配置和接口位于 `include/ai/`，CMake target 为 `rkmon_ai`。

`RKMON_WITH_RKNN` 控制 RKNN Runtime 和 YOLOv8 后处理；`RKMON_WITH_RGA` 控制 RGA letterbox。aarch64 默认开启，主机可关闭硬件依赖进行框架测试。

## 数据流

```text
NV12 frame -> RGA/CPU letterbox -> RKNN input -> YOLOv8 postprocess
           -> DetectionResult -> /run/rkmon/detections.json
                              -> recording event socket
```

配置指定模型、标签、输出快照、事件 Socket、推理 FPS、预处理方式、输入内存、worker 数和 NPU core policy。结果携带来源 frame sequence/timestamp、类别、置信度和框坐标；坐标需要从模型 letterbox 空间还原到原图空间。

检测快照采用原子更新供网页读取。录像事件通过 Unix Datagram Socket 尽力发送；录像服务不可用时不能阻塞视频和推理主链路，服务会通过目录和后续事件恢复录像索引，但丢失的瞬时检测事件不保证可恢复。

## 并发和故障

AI 使用独立有界队列和 worker，慢推理应丢弃旧待处理帧而不拖慢编码。RKNN 输出、RGA handle 和共享帧必须在使用期间保持所有权有效；不能把已释放的映射指针放入队列。推理异常通过服务故障回调上报。

## 验证边界

AI 单测覆盖模型输入、letterbox、后处理和结果写出；板端验证依赖 RKNN Runtime、模型摘要和 NPU。当前 AI 事件与录像关联已由 recording 集成测试覆盖，但长时间 NPU 稳定性、检测精度和多模型并行未作为工程保证。
