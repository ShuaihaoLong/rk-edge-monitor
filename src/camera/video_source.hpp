#pragma once

#include "media/frame.hpp"

#include <string>

namespace rkmon::camera {

// 采集请求；驱动最终接受的格式通过 NegotiatedFormat 返回。
struct CaptureConfig {
    std::string device;
    int width{1920};
    int height{1080};
    PixelFormat format{PixelFormat::MJPG};
    unsigned fps{30};
    unsigned buffer_count{4};              // 请求的驱动缓冲区数量，与业务队列容量独立。
    int poll_timeout_ms{1000};             // 单次 read 的等待时限。
    unsigned max_consecutive_timeouts{5};  // 由采集服务执行的连续超时故障阈值。
    unsigned reconnect_interval_ms{2000};  // 设备缺失或断开后的重新打开间隔。
};

// 帧间隔单位为秒，分子/分母为 0 时不能据此计算帧率。
struct NegotiatedFormat {
    int width{0};
    int height{0};
    PixelFormat format{PixelFormat::MJPG};
    std::size_t stride{0};
    std::size_t size_image{0};
    unsigned interval_numerator{0};
    unsigned interval_denominator{0};
};

enum class ReadStatus { frame, timeout, stopped, error };

// 仅 frame 状态携带有效帧；error 状态通过 error 字段返回原因。
struct ReadResult {
    ReadStatus status{ReadStatus::error};
    VideoFrame frame;
    std::string error;
};

// 视频源不负责创建采集线程，由调用方串行执行 open/read/close。
class IVideoSource {
public:
    virtual ~IVideoSource() = default;
    virtual void open() = 0;
    virtual ReadResult read() = 0;
    // 允许与 read 并发，唤醒阻塞读取；close 必须在读取线程结束后调用。
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept = 0;
    virtual NegotiatedFormat negotiated_format() const = 0;
};

} // namespace rkmon::camera
