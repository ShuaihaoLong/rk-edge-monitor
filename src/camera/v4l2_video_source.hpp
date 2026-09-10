#pragma once

#include "video_source.hpp"

#include <atomic>
#include <vector>

namespace rkmon::camera {

// 使用单平面 V4L2 API 和 MMAP 缓冲区采集视频。
class V4L2VideoSource final : public IVideoSource {
public:
    explicit V4L2VideoSource(CaptureConfig config);
    ~V4L2VideoSource() override;
    V4L2VideoSource(const V4L2VideoSource&) = delete;
    V4L2VideoSource& operator=(const V4L2VideoSource&) = delete;

    void open() override;
    ReadResult read() override;
    void request_stop() noexcept override;
    void close() noexcept override;
    NegotiatedFormat negotiated_format() const override {
        return actual_;
    }

private:
    struct Mapping {
        void* address;
        std::size_t length;
    };

    void query_capabilities();
    void configure_format();
    void configure_frame_interval();
    void allocate_buffers();

    // 配置请求与驱动协商结果分别保存，输出帧使用实际格式。
    CaptureConfig config_;
    NegotiatedFormat actual_;

    int fd_{-1};
    // 生命周期覆盖全部 read/request_stop，关闭设备时不销毁唤醒 fd。
    int wake_fd_{-1};
    bool streaming_{false};
    std::atomic<bool> stop_{false};
    // 按驱动缓冲区 index 索引；QBUF 后应用不得再使用其中的帧数据。
    std::vector<Mapping> mappings_;
};

} // namespace rkmon::camera
