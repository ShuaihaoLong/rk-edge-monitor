#include "media/rga_copy.hpp"
#include "media/nv12.hpp"
#include <im2d.h>
#include <stdexcept>
#include <string>
namespace rkmon::media {
namespace {
struct Import {
    rga_buffer_handle_t handle{};
    Import(int fd,int size):handle(importbuffer_fd(fd,size)) {
        if(!handle)throw std::runtime_error("RGA NV12 DMA import failed");
    }
    ~Import(){releasebuffer_handle(handle);}
};
}
void rga_copy_nv12(const camera::VideoFrame& f,const DmaBuffer& output,int stride,int height_stride) {
    validate_nv12(f);
    if(!f.dma || stride<f.width || height_stride<f.height || stride>32768 || height_stride>32768 ||
       stride%2 || height_stride%2 || output.size<static_cast<std::size_t>(stride)*height_stride*3/2)
        throw std::invalid_argument("invalid RGA NV12 copy layout");
    Import in(f.dma->fd,f.size),out(output.fd,output.size);
    auto src=wrapbuffer_handle_t(in.handle,f.width,f.height,f.stride,f.height_stride,RK_FORMAT_YCbCr_420_SP);
    auto dst=wrapbuffer_handle_t(out.handle,f.width,f.height,stride,height_stride,RK_FORMAT_YCbCr_420_SP);
    const auto status=imcopy(src,dst,1);
    if(status!=IM_STATUS_SUCCESS && status!=IM_STATUS_NOERROR)
        throw std::runtime_error(std::string("RGA NV12 copy: ")+imStrError_t(status));
}
}
