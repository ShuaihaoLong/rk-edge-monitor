#pragma once

#include "video/interfaces/video_decoder.hpp"

namespace rkmon::video {

// Pimpl 隔离 GStreamer 头文件、引用计数及管线状态。
class GstVideoPipeline final : public IVideoDecoder {
public:
    explicit GstVideoPipeline(DecodeConfig config);
    ~GstVideoPipeline() override;
    void open() override;
    std::optional<camera::VideoFrame> decode(const camera::VideoFrame& input) override;
    void request_stop() noexcept override;
    void close() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rkmon::video
