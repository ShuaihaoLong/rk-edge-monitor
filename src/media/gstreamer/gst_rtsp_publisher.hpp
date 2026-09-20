#pragma once
#include "media/publisher.hpp"

namespace rkmon::video {
class GstRtspPublisher final : public IVideoPublisher {
public:
    explicit GstRtspPublisher(StreamConfig config);
    ~GstRtspPublisher() override;
    void open() override;
    void write(const camera::VideoFrame& frame) override;
    void check_health() override;
    void request_stop() noexcept override;
    void close() noexcept override;
    std::uint64_t encoded_frames() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
