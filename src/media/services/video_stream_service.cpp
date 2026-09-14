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
    try {
        input_ = provider_();
        if (!input_ || input_->closed()) throw std::runtime_error("decoded frame queue is not available");
        publisher_->open();
        state_ = core::ServiceState::running;
        worker_ = std::thread(&VideoStreamService::run, this);
        return true;
    } catch (const std::exception& error) {
        publisher_->close();
        fail(error.what());
    } catch (...) {
        publisher_->close();
        fail("unknown stream startup failure");
    }
    return false;
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
    try {
        while (!stop_) {
            auto frame = input_->pop_for(std::chrono::milliseconds(20));
            if (stop_) break;
            // 即使输入暂时没有帧，也需要处理异步 RTSP 错误。
            publisher_->check_health();
            if (!frame) {
                if (input_->closed()) throw std::runtime_error("decoded frame queue closed unexpectedly");
                continue;
            }
            publisher_->write(*frame);
            ++submitted;
        }
    } catch (const std::exception& error) {
        if (!stop_) fail(error.what());
    } catch (...) {
        if (!stop_) fail("unknown stream worker failure");
    }
    if (state_ != core::ServiceState::failed) state_ = core::ServiceState::stopped;
    try {
        if (logger_) logger_->info("[video_stream] submitted={}, encoded={}", submitted, publisher_->encoded_frames());
    } catch (...) {}
}
}
