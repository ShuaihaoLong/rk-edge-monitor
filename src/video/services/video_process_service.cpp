#include "video/services/video_process_service.hpp"
#include <stdexcept>
#include <utility>

namespace rkmon::video {
VideoProcessService::VideoProcessService(std::unique_ptr<IVideoDecoder> decoder, InputProvider input,
                                       DecodeConfig config, FaultHandler fault,
                                       std::shared_ptr<spdlog::logger> logger, FrameSink sink)
    : decoder_(std::move(decoder)), provider_(std::move(input)), config_(config),
      fault_(std::move(fault)), sink_(std::move(sink)), logger_(std::move(logger)) {
    if (!decoder_ || !provider_ || config_.queue_capacity == 0) {
        throw std::invalid_argument("invalid video processing configuration");
    }
}

VideoProcessService::~VideoProcessService() {
    request_stop();
    join();
}

bool VideoProcessService::start() {
    if (running()) {
        return true;
    }
    if (worker_.joinable()) {
        throw std::logic_error("join decoder service before restarting");
    }
    stop_ = false;
    frames_ = 0;
    state_ = core::ServiceState::starting;
    {
        std::lock_guard lock(mutex_);
        error_.clear();
    }
    try {
        output_ = std::make_shared<Queue>(config_.queue_capacity);
        input_ = provider_();
        if (!input_ || input_->closed()) {
            throw std::runtime_error("capture queue is not available");
        }
        decoder_->open();
        state_ = core::ServiceState::running;
        worker_ = std::thread(&VideoProcessService::run, this);
        return true;
    } catch (const std::exception& error) {
        decoder_->close();
        fail(error.what());
    } catch (...) {
        decoder_->close();
        fail("unknown decoder startup failure");
    }
    return false;
}

void VideoProcessService::request_stop() noexcept {
    stop_ = true;
    auto expected = core::ServiceState::running;
    state_.compare_exchange_strong(expected, core::ServiceState::stopping);
    decoder_->request_stop();
    if (output_) {
        output_->close(core::CloseMode::discard);
    }
}

void VideoProcessService::join() noexcept {
    if (worker_.joinable()) {
        worker_.join();
    }
    // 工作线程不再访问管线，才能释放 GStreamer 对象。
    decoder_->close();
    if (state_ != core::ServiceState::failed) {
        state_ = core::ServiceState::stopped;
    }
}

bool VideoProcessService::running() const noexcept {
    return state_ == core::ServiceState::running;
}

core::HealthSnapshot VideoProcessService::health() const {
    std::lock_guard lock(mutex_);
    return {state_.load(), error_};
}

ProcessStats VideoProcessService::stats() const {
    return {frames_.load(), output_ ? output_->size() : 0, output_ ? output_->dropped() : 0};
}

void VideoProcessService::fail(std::string reason) noexcept {
    {
        std::lock_guard lock(mutex_);
        error_ = std::move(reason);
    }
    state_ = core::ServiceState::failed;
    if (output_) {
        output_->close(core::CloseMode::discard);
    }
    try {
        if (fault_) {
            fault_(health().detail);
        }
    } catch (...) {
        // 上报失败不能阻止工作线程退出。
    }
}

void VideoProcessService::run() noexcept {
    try {
        while (!stop_) {
            auto input = input_->pop_for(std::chrono::milliseconds(20));
            if (stop_) {
                break;
            }
            if (!input) {
                if (input_->closed()) {
                    throw std::runtime_error("capture queue closed unexpectedly");
                }
                continue;
            }
            auto frame = decoder_->decode(*input);
            if (stop_) {
                break;
            }
            if (!frame) {
                throw std::runtime_error("decoder stopped unexpectedly");
            }
            if (frames_ == 0 && logger_) {
                logger_->info("[video_decode] first NV12 frame: {}x{}, stride={}, bytes={}",
                              frame->width, frame->height, frame->stride, frame->size);
            }
            ++frames_;
            // 分发只读帧引用；订阅回调必须非阻塞，下游各自维护有界队列。
            if (sink_) sink_(*frame);
            // 下游慢时丢旧的原始图像，既不占住硬件输出，也不让内存无限增长。
            if (output_->try_push(std::move(*frame), core::OverflowPolicy::drop_oldest)
                == core::PushResult::closed) break;
        }
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("unknown video processing failure");
    }
    output_->close();
    if (state_ != core::ServiceState::failed) {
        state_ = core::ServiceState::stopped;
    }
    try {
        if (logger_) {
            logger_->info("[video_decode] frames={}, output_dropped={}", frames_.load(), output_->dropped());
        }
    } catch (...) {}
}
} // namespace rkmon::video
