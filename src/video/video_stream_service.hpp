#pragma once
#include "video_publisher.hpp"
#include "core/bounded_queue.hpp"
#include "core/service.hpp"
#include <spdlog/logger.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace rkmon::video {
class VideoStreamService final : public core::IService {
public:
    using Queue = core::BoundedQueue<camera::VideoFrame>;
    using InputProvider = std::function<std::shared_ptr<Queue>()>;
    using FaultHandler = std::function<void(const std::string&)>;
    VideoStreamService(std::unique_ptr<IVideoPublisher> publisher, InputProvider input,
                       FaultHandler fault = {}, std::shared_ptr<spdlog::logger> logger = {});
    ~VideoStreamService() override;
    bool start() override;
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override;
    std::string_view name() const noexcept override { return "video_stream"; }
    core::HealthSnapshot health() const override;
private:
    void run() noexcept;
    void fail(std::string reason) noexcept;
    std::unique_ptr<IVideoPublisher> publisher_;
    InputProvider provider_;
    FaultHandler fault_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<Queue> input_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<core::ServiceState> state_{core::ServiceState::stopped};
    mutable std::mutex mutex_;
    std::string error_;
};
}
