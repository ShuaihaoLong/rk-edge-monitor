#pragma once
#include "mqtt/config.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace rkmon::display {
struct SensorSnapshot {
    bool online{};
    double temperature{}, humidity{};
    std::string message{"等待 STM32 数据"};
};

// 只接受实时 telemetry；状态与样本分别计时，避免把 retained 历史值当成当前读数。
class SensorState {
public:
    explicit SensorState(unsigned stale_ms) : stale_ms_(stale_ms) {}

    void accept(const std::string& kind, const std::string& payload, bool retained);
    void disconnected();
    SensorSnapshot snapshot() const;

private:
    unsigned stale_ms_;
    bool connected_{}, online_{}, has_sample_{};
    double temperature_{}, humidity_{};
    std::chrono::steady_clock::time_point received_{};
};

class Telemetry {
public:
    Telemetry(mqtt::Config config, unsigned stale_ms);
    ~Telemetry();
    SensorSnapshot snapshot() const;

private:
    void run() noexcept;
    mqtt::Config config_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    SensorState state_;
    std::thread thread_;
};
}
