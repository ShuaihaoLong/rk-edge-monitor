#pragma once
#include "media/frame.hpp"
#include <vector>
namespace rkmon::ai {
struct ModelImage {
    std::vector<std::uint8_t> rgb;
    float scale{};
    int x_pad{}, y_pad{};
};
struct LetterboxGeometry {
    float scale;
    int width, height, x_pad, y_pad;
};
LetterboxGeometry nv12_letterbox_geometry(const camera::VideoFrame& frame, int size);
// 接受紧密排列或 DMA 对齐布局的 NV12；当前相机按 BT.601 limited range 转 RGB，保持宽高比并补灰边。
ModelImage nv12_letterbox(const camera::VideoFrame& frame, int size = 640);
void nv12_letterbox(const camera::VideoFrame& frame, ModelImage& output, int size = 640);
}
