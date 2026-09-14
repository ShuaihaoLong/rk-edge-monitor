#pragma once
#include "camera/videoFrame.hpp"
#include <vector>
namespace rkmon::ai {
struct ModelImage {
    std::vector<std::uint8_t> rgb;
    float scale{};
    int x_pad{}, y_pad{};
};
// 接受紧密排列的 NV12；当前相机按 BT.601 limited range 转 RGB，保持宽高比并补灰边。
ModelImage nv12_letterbox(const camera::VideoFrame& frame, int size = 640);
}
