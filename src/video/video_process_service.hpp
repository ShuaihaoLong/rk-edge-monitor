#pragma once

#include "video_decoder.hpp"
#include "core/bounded_queue.hpp"
#include "core/service.hpp"
#include <spdlog/logger.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace rkmon::video {

struct ProcessStats {
    std::uint64_t frames{0};
    std::size_t queue_depth{0};
    std::size_t dropped{0};
};

class VideoProcessService final : public core::IService {
public:
    using Queue = core::BoundedQueue<camera::VideoFrame>;
    // 在 start 时获取已启动上游的当前队列，避免重启后继续消费旧队列。
    using InputProvider = std::function<std::shared_ptr<Queue>()>;
    using FaultHandler = std::function<void(const std::string&)>;
    VideoProcessService(std::unique_ptr<IVideoDecoder> decoder, InputProvider input,
                        DecodeConfig config = {}, FaultHandler fault = {},
                        std::shared_ptr<spdlog::logger> logger = {});
    ~VideoProcessService() override;
    bool start() override;
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override;
    std::string_view name() const noexcept override {
        return "video_decode";
    }
    core::HealthSnapshot health() const override;
    // 控制线程在 start 成功后获取，停止后帧内存仍可独立持有。
    std::shared_ptr<Queue> output() const {
        return output_;
    }
    ProcessStats stats() const;
private:
    void run() noexcept;
    void fail(std::string reason) noexcept;
    std::unique_ptr<IVideoDecoder> decoder_;
    InputProvider provider_;
    DecodeConfig config_;
    FaultHandler fault_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<Queue> input_, output_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<core::ServiceState> state_{core::ServiceState::stopped};
    std::atomic<std::uint64_t> frames_{0};
    mutable std::mutex mutex_;
    std::string error_;
};
} // namespace rkmon::video
