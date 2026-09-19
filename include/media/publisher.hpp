#pragma once

#include "media/frame.hpp"
#include <memory>
#include <string>

namespace rkmon::video {
struct StreamConfig {
    std::string url{"rtsp://127.0.0.1:8554/camera"};
    unsigned bitrate{4000000};
    unsigned fps{30};
    unsigned gop{30};
    unsigned timeout_ms{5000};
    bool osd_enabled{true};
    std::string osd_timezone{"Asia/Shanghai"};
};

// 接收自有 NV12 帧；编码和传输由实现负责，服务层不接触媒体协议类型。
// open/write/check_health/close 串行调用，request_stop 可与 write 并发。
// 故障抛出异常后由调用方停止并 close；已编码数据不采用丢旧帧策略。
class IVideoPublisher {
public:
    virtual ~IVideoPublisher() = default;
    virtual void open() = 0;
    virtual void write(const camera::VideoFrame& frame) = 0;
    virtual void check_health() = 0;
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept = 0;
    virtual std::uint64_t encoded_frames() const noexcept = 0;
};
std::unique_ptr<IVideoPublisher> make_hardware_publisher(StreamConfig config);
} // namespace rkmon::video
