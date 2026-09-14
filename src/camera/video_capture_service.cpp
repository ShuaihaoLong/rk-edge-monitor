#include "video_capture_service.hpp"
#include "v4l2_video_source.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace rkmon::camera {

namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VideoCaptureService::VideoCaptureService(CaptureConfig config, std::size_t capacity,
                                       std::shared_ptr<spdlog::logger> logger)
    : VideoCaptureService(std::make_unique<V4L2VideoSource>(config), capacity,
                          config.max_consecutive_timeouts, std::move(logger),
                          config.reconnect_interval_ms) {}

VideoCaptureService::VideoCaptureService(std::unique_ptr<IVideoSource> source, std::size_t capacity,
                                       unsigned max_timeouts, std::shared_ptr<spdlog::logger> logger,
                                       unsigned reconnect_interval_ms)
    : source_(std::move(source)), queue_capacity_(capacity), max_timeouts_(max_timeouts),
      reconnect_interval_ms_(reconnect_interval_ms), logger_(std::move(logger)) {
    if (!source_ || capacity == 0 || max_timeouts == 0 || reconnect_interval_ms == 0) {
        throw std::invalid_argument("invalid capture service configuration");
    }
}

VideoCaptureService::~VideoCaptureService() {
    request_stop();
    join();
}

bool VideoCaptureService::start() {
    if (running()) {
        return true;
    }
    if (worker_.joinable()) {
        throw std::logic_error("join capture service before restarting");
    }
    state_ = core::ServiceState::starting;
    stop_ = false;
    frames_ = 0;
    timeouts_ = 0;
    ended_ns_ = 0;
    started_ns_ = now_ns();
    {
        std::lock_guard lock(health_mutex_);
        error_.clear();
    }
    // 队列在设备离线期间保持打开，下游服务因此无需随 USB 插拔重建。
    queue_ = std::make_shared<Queue>(queue_capacity_);
    online_ = false;
    stats_pending_ = true;
    state_ = core::ServiceState::running;
    try {
        worker_ = std::thread(&VideoCaptureService::run, this);
        return true;
    } catch (...) {
        state_ = core::ServiceState::failed;
        queue_->close(core::CloseMode::discard);
        throw;
    }
}

void VideoCaptureService::request_stop() noexcept {
    stop_ = true;
    online_ = false;
    auto expected = core::ServiceState::running;
    state_.compare_exchange_strong(expected, core::ServiceState::stopping);
    source_->request_stop();
    if (queue_) {
        queue_->close(core::CloseMode::discard);
    }
}

void VideoCaptureService::join() noexcept {
    if (worker_.joinable()) {
        worker_.join();
    }
    // read 已经结束，此时关闭设备不会与映射内存访问并发。
    source_->close();
    if (stats_pending_) {
        stats_pending_ = false;
        try {
            if (logger_) {
                const auto snapshot = stats();
                logger_->info("[video_capture] frames={}, fps={:.2f}, queue_dropped={}, timeouts={}",
                              snapshot.frames, snapshot.fps, snapshot.dropped, snapshot.timeouts);
            }
        } catch (...) {
            // 停止阶段的日志失败不能阻断资源回收。
        }
    }
    if (state_ != core::ServiceState::failed) {
        state_ = core::ServiceState::stopped;
    }
}

bool VideoCaptureService::running() const noexcept {
    return state_ == core::ServiceState::running;
}

core::HealthSnapshot VideoCaptureService::health() const {
    std::lock_guard lock(health_mutex_);
    return {state_.load(), error_};
}

CaptureStats VideoCaptureService::stats() const {
    const auto end = ended_ns_.load();
    const auto elapsed = (end ? end : now_ns()) - started_ns_.load();
    const auto frames = frames_.load();
    CaptureStats snapshot;
    snapshot.frames = frames;
    snapshot.timeouts = timeouts_.load();
    snapshot.queue_depth = queue_ ? queue_->size() : 0;
    snapshot.dropped = queue_ ? queue_->dropped() : 0;
    // 统计整个采集时段的平均帧率；停止后固定结束时间。
    if (elapsed > 0) {
        snapshot.fps = static_cast<double>(frames) * 1e9 / static_cast<double>(elapsed);
    }
    return snapshot;
}

void VideoCaptureService::run() noexcept {
    while (!stop_) {
        try {
            source_->open();
            online_ = true;
            {
                std::lock_guard lock(health_mutex_);
                error_.clear();
            }
            state_ = core::ServiceState::running;
            if (logger_) {
                const auto actual = source_->negotiated_format();
                logger_->info("[video_capture] online: {}x{}, format={}, stride={}, interval={}/{} s, queue_capacity={}",
                              actual.width, actual.height, static_cast<int>(actual.format), actual.stride,
                              actual.interval_numerator, actual.interval_denominator, queue_capacity_);
            }
            // 偶发超时允许继续采集，连续超时后关闭设备并重新枚举。
            unsigned consecutive_timeouts = 0;
            while (!stop_) {
                auto result = source_->read();
                if (stop_) {
                    break;
                }
                if (result.status == ReadStatus::stopped) {
                    throw std::runtime_error("video source stopped unexpectedly");
                }
                if (result.status == ReadStatus::error) {
                    throw std::runtime_error(result.error);
                }
                if (result.status == ReadStatus::timeout) {
                    ++timeouts_;
                    if (++consecutive_timeouts >= max_timeouts_) {
                        throw std::runtime_error("camera exceeded consecutive capture timeout limit");
                    }
                    continue;
                }
                consecutive_timeouts = 0;
                ++frames_;
                // 消费者跟不上时丢弃最旧帧，保持队列有界并优先提供近期画面。
                if (queue_->try_push(std::move(result.frame), core::OverflowPolicy::drop_oldest)
                    == core::PushResult::closed) {
                    break;
                }
            }
        } catch (const std::exception& error) {
            online_ = false;
            source_->close();
            {
                std::lock_guard lock(health_mutex_);
                error_ = error.what();
            }
            state_ = core::ServiceState::degraded;
            try {
                if (logger_) logger_->warn("[video_capture] offline: {}; retry in {}ms",
                                           error.what(), reconnect_interval_ms_);
            } catch (...) {}
            // 分段等待使 SIGTERM 不必等待完整重试周期。
            for (unsigned i = 0; i < (reconnect_interval_ms_ + 19) / 20 && !stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } catch (...) {
            online_ = false;
            source_->close();
            {
                std::lock_guard lock(health_mutex_);
                error_ = "unknown capture failure";
            }
            state_ = core::ServiceState::degraded;
            for (unsigned i = 0; i < (reconnect_interval_ms_ + 19) / 20 && !stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    online_ = false;
    source_->close();
    ended_ns_ = now_ns();
    queue_->close();
    if (state_ != core::ServiceState::failed) {
        state_ = core::ServiceState::stopped;
    }
}

} // namespace rkmon::camera
