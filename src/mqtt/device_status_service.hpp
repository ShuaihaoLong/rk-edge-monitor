#pragma once

#include "core/service.hpp"
#include "mqtt/config.hpp"
#include "mqtt_client.hpp"
#include <spdlog/logger.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace rkmon::mqtt {

class DeviceStatusService final : public core::IService {
public:
    using CameraStatus = std::function<bool()>;
    DeviceStatusService(Config config, CameraStatus camera_status,
                        std::shared_ptr<spdlog::logger> logger = {});
    ~DeviceStatusService() override;
    bool start() override;
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override { return running_.load(); }
    std::string_view name() const noexcept override { return "mqtt_status"; }
    core::HealthSnapshot health() const override;
private:
    void run() noexcept;
    std::string status_json(bool online);
    Config config_;
    CameraStatus camera_status_;
    std::shared_ptr<spdlog::logger> logger_;
    MqttClient client_;
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, connected_{false};
    mutable std::mutex mutex_;
    std::string detail_;
    std::uint64_t previous_total_{0}, previous_idle_{0};
};

} // namespace rkmon::mqtt
