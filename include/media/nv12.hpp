#pragma once
#include "media/frame.hpp"
#include <stdexcept>
namespace rkmon::media {
inline std::size_t nv12_uv_offset(const camera::VideoFrame& f) {
    return f.dma?f.uv_offset:f.stride*static_cast<std::size_t>(f.height);
}
inline void validate_nv12(const camera::VideoFrame& f) {
    if(f.width<=0 || f.height<=0 || f.width>16384 || f.height>16384 || f.width%2 || f.height%2 ||
       f.format!=camera::PixelFormat::NV12 || f.stride<static_cast<std::size_t>(f.width) || f.stride>32768)
        throw std::invalid_argument("invalid NV12 dimensions or stride");
    if(f.dma) {
        if(f.dma->fd<0 || !f.dma->owner || f.height_stride<static_cast<std::size_t>(f.height) ||
           f.height_stride>32768 || f.height_stride%2 || f.uv_offset!=f.stride*f.height_stride ||
           f.size<f.stride*f.height_stride*3/2 || f.dma->size<f.size)
            throw std::invalid_argument("invalid DMA NV12 plane layout");
    } else if(!f.data || f.stride!=static_cast<std::size_t>(f.width) ||
              f.size!=f.stride*static_cast<std::size_t>(f.height)*3/2)
        throw std::invalid_argument("invalid packed NV12 buffer");
}
}
