# Rockchip YOLOv8 配套后处理

来自 RKNN Model Zoo v2.1.0，固定提交：
`c2b7d00714b4e5d21266ab3003f3ca687ba0d57b`。
来源：https://github.com/airockchip/rknn_model_zoo/tree/c2b7d00714b4e5d21266ab3003f3ca687ba0d57b

保留原文件内容，展开到本目录：
- `examples/yolov8/cpp/{postprocess.cc,postprocess.h,yolov8.h}`
- `utils/{common.h,image_utils.h}`
- `3rdparty/rknpu2/include/rknn_api.h`
- 根目录 `LICENSE`

生产检测器只调用 `post_process`，标签由实例独立读取，不调用上游基于全局变量的标签加载函数。
仅适配当前固定的九输出 INT8 模型；更换导出格式须同步验证张量布局和解码逻辑。
