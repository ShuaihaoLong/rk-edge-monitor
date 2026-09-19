#pragma once

#include "ai/letterbox/nv12_letterbox.hpp"
#include <memory>

namespace rkmon::ai {
// 单线程使用；返回的图像归预处理器所有，有效期至下一次 process 或析构。
class RgaLetterbox {
public:
    explicit RgaLetterbox(int size = 640);
    // 外部 RGB DMA-BUF 的所有者必须活到预处理器销毁之后。
    RgaLetterbox(int size, int output_fd, std::size_t output_bytes, int width_stride);
    ~RgaLetterbox();
    RgaLetterbox(const RgaLetterbox&) = delete;
    RgaLetterbox& operator=(const RgaLetterbox&) = delete;
    const ModelImage& process(const camera::VideoFrame& frame);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rkmon::ai
