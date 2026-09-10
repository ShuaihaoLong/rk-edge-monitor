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
                                       FaultHandler handler, std::shared_ptr<spdlog::logger> logger)
    : VideoCaptureService(std::make_unique<V4L2VideoSource>(config), capacity,
                          config.max_consecutive_timeouts, std::move(handler), std::move(logger)) {}

VideoCaptureService::VideoCaptureService(std::unique_ptr<IVideoSource> source, std::size_t capacity,
                                       unsigned max_timeouts, FaultHandler handler,
                                       std::shared_ptr<spdlog::logger> logger)
    : source_(std::move(source)), queue_capacity_(capacity), max_timeouts_(max_timeouts),
      on_fault_(std::move(handler)), logger_(std::move(logger)) {
    if (!source_ || capacity == 0 || max_timeouts == 0) {
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
    try {
        // 已关闭的队列不能复用，重启时为消费者提供新队列。
        queue_ = std::make_shared<Queue>(queue_capacity_);
        source_->open();
        stats_pending_ = true;
        if (logger_) {
            const auto actual = source_->negotiated_format();
            logger_->info("[video_capture] {}x{}, format={}, stride={}, interval={}/{} s, queue_capacity={}",
                          actual.width, actual.height, static_cast<int>(actual.format), actual.stride,
                          actual.interval_numerator, actual.interval_denominator, queue_capacity_);
        }
        started_ns_ = now_ns();
        state_ = core::ServiceState::running;
        // 设备初始化成功后再启动读取线程。
        worker_ = std::thread(&VideoCaptureService::run, this);
        return true;
    } catch (const std::exception& error) {
        source_->close();
        fail(error.what());
    } catch (...) {
        source_->close();
        fail("unknown capture startup failure");
    }
    return false;
}

void VideoCaptureService::request_stop() noexcept {
    stop_ = true;
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

void VideoCaptureService::fail(std::string reason) noexcept {
    {
        std::lock_guard lock(health_mutex_);
        error_ = std::move(reason);
    }
    // 先发布故障并唤醒消费者，再通知应用控制线程。
    state_ = core::ServiceState::failed;
    if (queue_) {
        queue_->close(core::CloseMode::discard);
    }
    try {
        if (on_fault_) {
            on_fault_(health().detail);
        }
    } catch (...) {
        // 故障回调失败不能阻断采集线程退出。
    }
}

void VideoCaptureService::run() noexcept {
    try {
        // 偶发超时允许继续采集，只有连续达到阈值才升级为服务故障。
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
        fail(error.what());
    } catch (...) {
        fail("unknown capture worker failure");
    }
    ended_ns_ = now_ns();
    queue_->close();
    if (state_ != core::ServiceState::failed) {
        state_ = core::ServiceState::stopped;
    }
}

} // namespace rkmon::camera
