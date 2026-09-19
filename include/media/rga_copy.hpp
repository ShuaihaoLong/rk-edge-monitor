#pragma once
#include "media/frame.hpp"
namespace rkmon::media {
// 同步 NV12 DMA 拷贝到独占目标；CPU OSD 必须在返回后才访问目标。
void rga_copy_nv12(const camera::VideoFrame& source,const DmaBuffer& destination,
                   int width_stride,int height_stride);
}
