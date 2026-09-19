#pragma once

#include "ai/nv12_letterbox.hpp"
#include <memory>

namespace rkmon::ai {
// 单线程使用；返回的图像归预处理器所有，有效期至下一次 process 或析构。
class RgaLetterbox {
public:
    explicit RgaLetterbox(int size = 640);
    ~RgaLetterbox();
    RgaLetterbox(const RgaLetterbox&) = delete;
    RgaLetterbox& operator=(const RgaLetterbox&) = delete;
    const ModelImage& process(const camera::VideoFrame& frame);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rkmon::ai
