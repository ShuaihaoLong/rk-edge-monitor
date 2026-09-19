#pragma once

#include "media/frame.hpp"
#include <memory>
#include <optional>

namespace rkmon::video {

struct DecodeConfig {
    unsigned timeout_ms{2000};
    std::size_t queue_capacity{4};
};

// 一次提交一张完整 JPEG，成功返回对应的 NV12 图像；失败抛出异常。
// 硬件输出持有解码池 DMA-BUF；按 stride/uv_offset/height_stride 访问，不假设紧密排列。
// 仅支持偶数宽高；sequence/timestamp 保留输入值，不使用解码器重建的时间戳。
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual void open() = 0;
    virtual std::optional<camera::VideoFrame> decode(const camera::VideoFrame& input) = 0;
    // 可与 decode 并发；停止后 decode 返回空值。close 必须在 decode 结束后调用。
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept = 0;
};

std::unique_ptr<IVideoDecoder> make_hardware_decoder(DecodeConfig config);

} // namespace rkmon::video
