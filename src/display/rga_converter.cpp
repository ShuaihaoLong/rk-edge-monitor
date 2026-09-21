#include "rga_converter.hpp"
#include "media/dma_buffer.hpp"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <im2d.h>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rkmon::display {
namespace {

void check(IM_STATUS status) {
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR)
        throw std::runtime_error(std::string("display RGA: ") + imStrError_t(status));
}

struct Import {
    rga_buffer_handle_t handle;

    Import(int fd, int size) : handle(importbuffer_fd(fd, size)) {
        if (!handle)
            throw std::runtime_error("display RGA DMA import failed");
    }

    ~Import() {
        releasebuffer_handle(handle);
    }

    Import(const Import&) = delete;
    Import& operator=(const Import&) = delete;
};

}

struct RgaConverter::Impl {
    std::shared_ptr<media::DmaBuffer> buffer = media::allocate_dma_buffer(768 * 432 * 4);
    // 导入句柄必须先于底层 DMA 缓冲区释放。
    Import output{buffer->fd, static_cast<int>(buffer->size)};
};

RgaConverter::RgaConverter() : impl_(std::make_unique<Impl>()) {
    check(imcheckHeader());
}

RgaConverter::~RgaConverter() = default;

VideoImage RgaConverter::convert(GstSample* sample) {
    GstVideoInfo info{};
    auto* buffer = gst_sample_get_buffer(sample);
    auto* meta = buffer ? gst_buffer_get_video_meta(buffer) : nullptr;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) || !meta ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
        meta->format != GST_VIDEO_FORMAT_NV12 || meta->n_planes != 2 ||
        gst_buffer_n_memory(buffer) != 1 || meta->offset[0] != 0 ||
        meta->stride[0] <= 0 || meta->stride[0] != meta->stride[1] ||
        meta->offset[1] % meta->stride[0] != 0)
        throw std::runtime_error("display requires linear single-buffer NV12 DMA layout");

    auto* memory = gst_buffer_peek_memory(buffer, 0);
    gsize offset = 0;
    const auto size = gst_memory_get_sizes(memory, &offset, nullptr);
    const auto height_stride = meta->offset[1] / meta->stride[0];
    const int width = GST_VIDEO_INFO_WIDTH(&info), height = GST_VIDEO_INFO_HEIGHT(&info);
    if (!gst_is_dmabuf_memory(memory) || offset || size > INT_MAX ||
        width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        width % 2 || height % 2 || meta->stride[0] > 32768 ||
        meta->width != static_cast<unsigned>(width) ||
        meta->height != static_cast<unsigned>(height) ||
        meta->stride[0] < width || height_stride < static_cast<unsigned>(height) ||
        height_stride > 32768 || height_stride % 2 ||
        size < meta->offset[1] + height_stride / 2 * meta->stride[1])
        throw std::runtime_error("invalid display NV12 DMA strides or allocation");

    Import input(gst_dmabuf_memory_get_fd(memory), static_cast<int>(size));
    auto src = wrapbuffer_handle_t(input.handle, width, height, meta->stride[0],
                                   static_cast<int>(height_stride), RK_FORMAT_YCbCr_420_SP);
    auto dst = wrapbuffer_handle_t(impl_->output.handle, 768, 432, 768, 432, RK_FORMAT_BGRA_8888);
    const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    const bool bt709 = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709;
    src.color_space_mode = bt709 ? (full ? IM_YUV_BT709_FULL_RANGE : IM_YUV_BT709_LIMIT_RANGE)
                                : (full ? IM_YUV_BT601_FULL_RANGE : IM_YUV_BT601_LIMIT_RANGE);
    dst.color_space_mode = IM_RGB_FULL;
    im_opt_t options{};
    // 解码池可能位于 4 GiB 以上；使用导入句柄并指定支持该地址范围的 RGA3。
    options.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
    check(improcess(src, dst, {}, {0, 0, width, height}, {0, 0, 768, 432}, {},
                    -1, nullptr, &options, IM_SYNC));

    VideoImage result;
    result.pixels.resize(768 * 432 * 4);
    media::DmaMapping mapped(impl_->buffer);
    std::memcpy(result.pixels.data(), mapped.data(), result.pixels.size());
    mapped.finish();
    result.received = std::chrono::steady_clock::now();
    return result;
}

}
