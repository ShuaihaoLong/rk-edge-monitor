#include "media/services/video_stream_service.hpp"
#include <stdexcept>
#include <utility>

namespace rkmon::video {
VideoStreamService::VideoStreamService(std::unique_ptr<IVideoPublisher> publisher, InputProvider input,
                                     FaultHandler fault, std::shared_ptr<spdlog::logger> logger)
    : publisher_(std::move(publisher)), provider_(std::move(input)),
      fault_(std::move(fault)), logger_(std::move(logger)) {
    if (!publisher_ || !provider_) throw std::invalid_argument("invalid stream service configuration");
}
VideoStreamService::~VideoStreamService() {
    request_stop();
    join();
}
bool VideoStreamService::start() {
    if (running()) return true;
    if (worker_.joinable()) throw std::logic_error("join stream service before restarting");
    stop_ = false;
    state_ = core::ServiceState::starting;
    {
        std::lock_guard lock(mutex_);
        error_.clear();
    }
    input_ = provider_();
    if (!input_ || input_->closed()) {
        fail("decoded frame queue is not available");
        return false;
    }
    state_ = core::ServiceState::running;
    try {
        worker_ = std::thread(&VideoStreamService::run, this);
        return true;
    } catch (...) {
        state_ = core::ServiceState::failed;
        throw;
    }
}
void VideoStreamService::request_stop() noexcept {
    stop_ = true;
    auto expected = core::ServiceState::running;
    state_.compare_exchange_strong(expected, core::ServiceState::stopping);
    publisher_->request_stop();
}
void VideoStreamService::join() noexcept {
    if (worker_.joinable()) worker_.join();
    publisher_->close();
    if (state_ != core::ServiceState::failed) state_ = core::ServiceState::stopped;
}
bool VideoStreamService::running() const noexcept { return state_ == core::ServiceState::running; }
core::HealthSnapshot VideoStreamService::health() const {
    std::lock_guard lock(mutex_);
    return {state_.load(), error_};
}
void VideoStreamService::fail(std::string reason) noexcept {
    {
        std::lock_guard lock(mutex_);
        error_ = std::move(reason);
    }
    state_ = core::ServiceState::failed;
    try {
        if (fault_) fault_(health().detail);
    } catch (...) {
        // 外部故障回调不能阻止线程退出和管线回收。
    }
}
void VideoStreamService::run() noexcept {
    std::uint64_t submitted = 0;
    std::uint64_t generation = 0;
    while (!stop_) {
        try {
            publisher_->open();
            {
                std::lock_guard lock(mutex_);
                error_.clear();
            }
            state_ = core::ServiceState::running;
            generation = 0;
            while (!stop_) {
                auto frame = input_->pop_for(std::chrono::milliseconds(20));
                if (stop_) break;
                if (!frame) {
                    if (input_->closed()) throw std::runtime_error("decoded frame queue closed unexpectedly");
                    continue;
                }
                if (generation != 0 && frame->source_generation != generation) {
                    // 新摄像头会话不能沿用旧编码器的参考帧和 RTSP 时间线。
                    publisher_->close();
                    if (stop_) break;
                    publisher_->open();
                    if (stop_) {
                        publisher_->request_stop();
                        break;
                    }
                    if (logger_) logger_->info("[video_stream] camera session changed; publisher restarted");
                }
                generation = frame->source_generation;
                publisher_->write(*frame);
                ++submitted;
            }
        } catch (const std::exception& error) {
            if (stop_) break;
            {
                std::lock_guard lock(mutex_);
                error_ = error.what();
            }
            state_ = core::ServiceState::degraded;
            try {
                if (logger_) logger_->warn("[video_stream] {}; retry in 2s", error.what());
            } catch (...) {}
        } catch (...) {
            if (stop_) break;
            {
                std::lock_guard lock(mutex_);
                error_ = "unknown stream failure";
            }
            state_ = core::ServiceState::degraded;
        }
        publisher_->close();
        for (unsigned i = 0; i < 100 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    publisher_->close();
    state_ = core::ServiceState::stopped;
    try {
        if (logger_) logger_->info("[video_stream] submitted={}, encoded={}", submitted, publisher_->encoded_frames());
    } catch (...) {}
}
}
