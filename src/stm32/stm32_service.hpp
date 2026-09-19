#pragma once

#include "core/service.hpp"
#include "mqtt/config.hpp"
#include "stm32/config.hpp"
#include "stm32/protocol.hpp"
#include <spdlog/logger.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace rkmon::stm32 {

struct Dht11Sample {
    std::uint32_t sequence{};
    std::uint8_t temp_int{}, temp_dec{}, humi_int{}, humi_dec{};
    std::int64_t received_at_ms{};
};
std::optional<Dht11Sample> decode_dht11(const Frame& frame, std::int64_t received_at_ms);
std::string telemetry_json(const Dht11Sample& sample);

class Stm32Service final : public core::IService {
public:
    Stm32Service(Config config, mqtt::Config mqtt, std::shared_ptr<spdlog::logger> logger = {});
    ~Stm32Service() override;
    bool start() override;
    void request_stop() noexcept override;
    void join() noexcept override;
    bool running() const noexcept override { return running_.load(); }
    std::string_view name() const noexcept override { return "stm32"; }
    core::HealthSnapshot health() const override;
private:
    void serial_loop() noexcept;
    void mqtt_loop() noexcept;
    void record_frame(const Frame& frame);
    void enqueue_command(const std::string& payload, bool retained);
    void result(std::uint32_t sequence, const char* state);
    void event(std::string topic, std::string payload);
    std::string status_json(bool shutting_down = false) const;
    bool online_locked() const;
    void warn(const char* component, const std::exception& error) const noexcept;

    Config config_;
    mqtt::Config mqtt_;
    std::string prefix_;
    std::shared_ptr<spdlog::logger> logger_;
    std::thread serial_thread_, mqtt_thread_;
    std::atomic<bool> stop_{false}, running_{false}, mqtt_connected_{false};
    mutable std::mutex mutex_;
    bool serial_connected_{false};
    std::optional<Dht11Sample> sample_;
    std::chrono::steady_clock::time_point last_sample_{};
    std::uint64_t sample_generation_{0};
    std::uint32_t next_sequence_{0};
    std::deque<Frame> commands_;
    struct Event { std::uint64_t id; std::string topic, payload; };
    std::deque<Event> events_;
    std::uint64_t next_event_{0};
};

} // namespace rkmon::stm32
