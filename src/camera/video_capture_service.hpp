#pragma once

#include "video_source.hpp"
#include "core/bounded_queue.hpp"
#include "core/service.hpp"

#include <spdlog/logger.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace rkmon::camera {

struct CaptureStats {
    std::uint64_t frames{0};
    std::uint64_t timeouts{0};
    std::size_t queue_depth{0};
    std::size_t dropped{0};
    double fps{0};
};

// 管理采集线程和输出队列；设备访问由 IVideoSource 实现。
// start/request_stop/join 由控制线程按生命周期顺序调用。
class VideoCaptureService final : public core::IService {
public:
    using Queue = core::BoundedQueue<VideoFrame>;
    explicit VideoCaptureService(CaptureConfig config, std::size_t queue_capacity = 4,
                                 std::shared_ptr<spdlog::logger> logger = {});
    VideoCaptureService(std::unique_ptr<IVideoSource> source, std::size_t queue_capacity = 4,
                        unsigned max_consecutive_timeouts = 5,
                        std::shared_ptr<spdlog::logger> logger = {},
                        unsigned reconnect_interval_ms = 2000);
    ~VideoCaptureService() override;

    bool start() override;
    // 通知读取结束并关闭输出队列；join 等待线程退出后再关闭设备。
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override;
    std::string_view name() const noexcept override {
        return "video_capture";
    }
    core::HealthSnapshot health() const override;

    // 控制线程在 start 成功后获取；每次重启创建新队列，旧句柄保持关闭状态。
    std::shared_ptr<Queue> output() const {
        return queue_;
    }
    CaptureStats stats() const;
    // true 仅表示设备当前已打开且最近一次读取未发生致命错误。
    [[nodiscard]] bool online() const noexcept {
        return online_.load();
    }
    NegotiatedFormat negotiated_format() const {
        return source_->negotiated_format();
    }

private:
    void run() noexcept;
    std::unique_ptr<IVideoSource> source_;
    std::size_t queue_capacity_;
    unsigned max_timeouts_;
    unsigned reconnect_interval_ms_;
    std::shared_ptr<spdlog::logger> logger_;
    bool stats_pending_{false};
    std::shared_ptr<Queue> queue_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> online_{false};
    std::atomic<core::ServiceState> state_{core::ServiceState::stopped};

    mutable std::mutex health_mutex_;
    std::string error_;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::int64_t> started_ns_{0};
    std::atomic<std::int64_t> ended_ns_{0};
};

} // namespace rkmon::camera
