#pragma once
#include "video_reader.hpp"
#include <gst/gst.h>
#include <memory>

namespace rkmon::display {

class RgaConverter {
public:
    RgaConverter();
    ~RgaConverter();

    RgaConverter(const RgaConverter&) = delete;
    RgaConverter& operator=(const RgaConverter&) = delete;

    // 同步转换期间调用方必须保有 sample，避免解码池提前复用源帧。
    VideoImage convert(GstSample* sample);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
