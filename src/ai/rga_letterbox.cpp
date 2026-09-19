#include "ai/rga_letterbox.hpp"

#include <im2d.h>
#include <stdexcept>
#include <string>

namespace rkmon::ai {
namespace {
void require_status(IM_STATUS status, const char* operation) {
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR)
        throw std::runtime_error(std::string("RGA ") + operation + ": " + imStrError_t(status));
}
class ImportedBuffer {
public:
    ~ImportedBuffer() { if (handle_) releasebuffer_handle(handle_); }
    ImportedBuffer() = default;
    ImportedBuffer(const ImportedBuffer&) = delete;
    ImportedBuffer& operator=(const ImportedBuffer&) = delete;
    void import(void* address, int size) {
        if (handle_) throw std::logic_error("RGA buffer already imported");
        handle_ = importbuffer_virtualaddr(address, size);
        if (!handle_) throw std::runtime_error("RGA virtual buffer import failed");
    }
    rga_buffer_handle_t get() const noexcept { return handle_; }
private:
    rga_buffer_handle_t handle_{};
};
}

struct RgaLetterbox::Impl {
    int size;
    ModelImage image;
    // handle 必须先于 image 释放，驱动导入期间 image.rgb 不得重新分配。
    ImportedBuffer output;
    int source_width{}, source_height{};
    explicit Impl(int value) : size(value) {}
};

RgaLetterbox::RgaLetterbox(int size) : impl_(std::make_unique<Impl>(size)) {
    if (size < 16 || size > 4096 || size % 16)
        throw std::invalid_argument("RGA model size must be a multiple of 16 in [16,4096]");
    require_status(imcheckHeader(), "header compatibility");
    impl_->image.rgb.resize(static_cast<std::size_t>(size) * size * 3);
    impl_->output.import(impl_->image.rgb.data(), static_cast<int>(impl_->image.rgb.size()));
}
RgaLetterbox::~RgaLetterbox() = default;

const ModelImage& RgaLetterbox::process(const camera::VideoFrame& frame) {
    auto& s = *impl_;
    const auto geometry = nv12_letterbox_geometry(frame, s.size);
    // 源帧地址每次可能改变；导入仅覆盖本次同步任务，不缓存可能已被回收的地址。
    ImportedBuffer input;
    input.import(const_cast<std::uint8_t*>(frame.data.get()), static_cast<int>(frame.size));
    auto src = wrapbuffer_handle_t(input.get(), frame.width, frame.height,
                                   frame.width, frame.height, RK_FORMAT_YCbCr_420_SP);
    auto dst = wrapbuffer_handle_t(s.output.get(), s.size, s.size, s.size, s.size, RK_FORMAT_RGB_888);
    dst.color_space_mode = IM_YUV_TO_RGB_BT601_LIMIT;
    const im_rect source{0, 0, frame.width, frame.height};
    const im_rect destination{geometry.x_pad, geometry.y_pad, geometry.width, geometry.height};
    require_status(imcheck_t(src, dst, {}, source, destination, {}, IM_SYNC), "letterbox parameters");
    if (s.source_width != frame.width || s.source_height != frame.height) {
        // 只在首次使用或源尺寸变化时清灰边；每个后续任务完整覆盖有效图像矩形。
        // RGB 纯色填充不进行颜色转换，不能沿用 NV12 转换任务的 CSC 参数。
        auto fill_dst = dst;
        fill_dst.color_space_mode = 0;
        require_status(imfill(fill_dst, {0, 0, s.size, s.size}, 0xff727272, 1), "padding fill");
    }
    im_opt_t options{};
    require_status(improcess(src, dst, {}, source, destination, {}, -1, nullptr, &options, IM_SYNC),
                   "NV12 resize and RGB conversion");
    s.source_width = frame.width;
    s.source_height = frame.height;
    s.image.scale = geometry.scale;
    s.image.x_pad = geometry.x_pad;
    s.image.y_pad = geometry.y_pad;
    return s.image;
}
} // namespace rkmon::ai
