#pragma once
#include "ai/interfaces/object_detector.hpp"
#include "ai/transport/result_writer.hpp"
#include "core/service.hpp"
#include "core/bounded_queue.hpp"
#include <spdlog/logger.h>
#include <atomic>
#include <mutex>
#include <thread>
namespace rkmon::ai {
class InferenceService final : public core::IService {
public:
    using Queue=core::BoundedQueue<camera::VideoFrame>;
    InferenceService(std::unique_ptr<IObjectDetector>, std::shared_ptr<Queue>, InferenceConfig,
                     std::shared_ptr<spdlog::logger> logger={});
    ~InferenceService() override;
    bool start() override;
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override;
    std::string_view name() const noexcept override { return "ai_inference"; }
    core::HealthSnapshot health() const override;
private:
    void run() noexcept;
    void publish(const std::string&,const DetectionResult* =nullptr) noexcept;
    std::unique_ptr<IObjectDetector> detector_;
    std::shared_ptr<Queue> input_;
    InferenceConfig config_;
    ResultWriter writer_;
    std::shared_ptr<spdlog::logger> logger_;
    std::thread worker_;
    std::atomic<bool> stop_{false},running_{false};
    mutable std::mutex mutex_;
    std::string detail_;
};
}
