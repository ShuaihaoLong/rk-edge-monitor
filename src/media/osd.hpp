#pragma once

#include "media/frame.hpp"
#include <string>

namespace rkmon::video {
std::string osd_text(const camera::VideoFrame& frame, const std::string& timezone);
// 只写入编码分支独占的 NV12 平面，支持行填充，不修改共享源帧。
void draw_osd(std::uint8_t* y, std::size_t y_stride, std::uint8_t* uv, std::size_t uv_stride,
              int width, int height, const std::string& text) noexcept;
} // namespace rkmon::video
