#include "stm32_service.hpp"
#include "serial_port.hpp"
#include "mqtt/mqtt_client.hpp"

#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace rkmon::stm32 {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (auto byte : bytes) { value += digits[byte >> 4]; value += digits[byte & 15]; }
    return value;
}
void pause(const std::atomic<bool>& stop, unsigned milliseconds) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stop && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
}

std::optional<Dht11Sample> decode_dht11(const Frame& frame, std::int64_t received_at_ms) {
    if (frame.type != msg_dht11_data || frame.payload.size() != 4) return std::nullopt;
    const auto& p = frame.payload;
    if (p[0] > 50 || p[1] > 9 || p[2] > 100 || p[3] > 9 ||
        (p[0] == 50 && p[1]) || (p[2] == 100 && p[3])) return std::nullopt;
    return Dht11Sample{frame.sequence, p[0], p[1], p[2], p[3], received_at_ms};
}

std::string telemetry_json(const Dht11Sample& sample) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"schema\":1,\"sequence\":" << sample.sequence
        << ",\"temperature_c\":" << unsigned(sample.temp_int) << '.' << unsigned(sample.temp_dec)
        << ",\"humidity_percent\":" << unsigned(sample.humi_int) << '.' << unsigned(sample.humi_dec)
        << ",\"raw\":[" << unsigned(sample.temp_int) << ',' << unsigned(sample.temp_dec)
        << ',' << unsigned(sample.humi_int) << ',' << unsigned(sample.humi_dec)
        << "],\"received_at_ms\":" << sample.received_at_ms << '}';
    return out.str();
}

Stm32Service::Stm32Service(Config config, mqtt::Config mqtt, std::shared_ptr<spdlog::logger> logger)
    : config_(std::move(config)), mqtt_(std::move(mqtt)),
      prefix_(mqtt_.topic_prefix + "/" + mqtt_.device_id + "/stm32"), logger_(std::move(logger)) {
    if (config_.device.empty() || !config_.frame_timeout_ms || !config_.stale_timeout_ms ||
        !config_.reconnect_interval_ms || mqtt_.device_id.empty())
        throw std::invalid_argument("invalid STM32 service configuration");
}

Stm32Service::~Stm32Service() { request_stop(); join(); }
bool Stm32Service::start() {
    if (running_) return true;
    if (serial_thread_.joinable() || mqtt_thread_.joinable()) throw std::logic_error("join STM32 service before restart");
    {
        std::lock_guard lock(mutex_);
        commands_.clear(); events_.clear(); sample_.reset(); serial_connected_ = false;
    }
    stop_ = false; running_ = true; mqtt_connected_ = false;
    try {
        serial_thread_ = std::thread(&Stm32Service::serial_loop, this);
        mqtt_thread_ = std::thread(&Stm32Service::mqtt_loop, this);
    } catch (...) { request_stop(); join(); throw; }
    return true;
}
void Stm32Service::request_stop() noexcept { stop_ = true; }
void Stm32Service::join() noexcept {
    if (serial_thread_.joinable()) serial_thread_.join();
    if (mqtt_thread_.joinable()) mqtt_thread_.join();
    running_ = false;
}
bool Stm32Service::online_locked() const {
    return serial_connected_ && sample_ && Clock::now() - last_sample_ < std::chrono::milliseconds(config_.stale_timeout_ms);
}
core::HealthSnapshot Stm32Service::health() const {
    if (!running_) return {core::ServiceState::stopped, {}};
    std::lock_guard lock(mutex_);
    if (!online_locked()) return {core::ServiceState::degraded, "STM32 data unavailable or stale"};
    if (!mqtt_connected_) return {core::ServiceState::degraded, "STM32 MQTT disconnected"};
    return {core::ServiceState::running, {}};
}
std::string Stm32Service::status_json(bool shutting_down) const {
    std::lock_guard lock(mutex_);
    return std::string("{\"schema\":1,\"online\":") + (!shutting_down && online_locked() ? "true" : "false") +
        ",\"serial_connected\":" + (!shutting_down && serial_connected_ ? "true" : "false") +
        ",\"stale_timeout_ms\":" + std::to_string(config_.stale_timeout_ms) +
        ",\"updated_at_ms\":" + std::to_string(unix_ms()) + '}';
}
void Stm32Service::warn(const char* component, const std::exception& error) const noexcept {
    try { if (logger_) logger_->warn("[stm32/{}] {}", component, error.what()); } catch (...) {}
}
void Stm32Service::event(std::string topic, std::string payload) {
    // 调用方持有 mutex_；断网时仅保留最近 32 条事件。
    if (events_.size() == 32) events_.pop_front();
    events_.push_back({++next_event_, std::move(topic), std::move(payload)});
}
void Stm32Service::result(std::uint32_t sequence, const char* state) {
    event(prefix_ + "/command_result", "{\"schema\":1,\"sequence\":" + std::to_string(sequence) +
          ",\"state\":\"" + state + "\",\"updated_at_ms\":" + std::to_string(unix_ms()) + '}');
}
void Stm32Service::enqueue_command(const std::string& payload, bool retained) {
    std::lock_guard lock(mutex_);
    const auto sequence = next_sequence_++;
    if (retained) { result(sequence, "rejected_retained"); return; }
    if (payload.size() > max_payload) { result(sequence, "rejected_length"); return; }
    if (!online_locked()) { result(sequence, "rejected_offline"); return; }
    if (commands_.size() >= 16) { result(sequence, "rejected_busy"); return; }
    commands_.push_back({msg_cmd_ctrl, sequence, {payload.begin(), payload.end()}});
}
void Stm32Service::record_frame(const Frame& frame) {
    std::lock_guard lock(mutex_);
    if (auto sample = decode_dht11(frame, unix_ms())) {
        sample_ = *sample; last_sample_ = Clock::now(); ++sample_generation_;
    } else if (frame.type == msg_ack) {
        event(prefix_ + "/ack", "{\"schema\":1,\"sequence\":" + std::to_string(frame.sequence) +
              ",\"payload_hex\":\"" + hex(frame.payload) + "\",\"received_at_ms\":" + std::to_string(unix_ms()) + '}');
    }
}
void Stm32Service::serial_loop() noexcept {
    SerialPort port;
    FrameParser parser{std::chrono::milliseconds(config_.frame_timeout_ms)};
    while (!stop_) {
        try {
            if (!port.connected()) {
                port.open(config_.device, config_.baud_rate); parser.reset();
                std::lock_guard lock(mutex_);
                serial_connected_ = true; sample_.reset();
            }
            std::uint8_t bytes[512];
            const auto count = port.read(bytes, sizeof(bytes), 20);
            for (const auto& frame : parser.feed(bytes, count)) record_frame(frame);
            std::optional<Frame> command;
            {
                std::lock_guard lock(mutex_);
                if (!commands_.empty()) { command = std::move(commands_.front()); commands_.pop_front(); }
                if (command && !online_locked()) { result(command->sequence, "rejected_offline"); command.reset(); }
            }
            if (command) {
                try {
                    const auto frame = encode(*command);
                    port.write(frame.data(), frame.size(), 200);
                    std::lock_guard lock(mutex_);
                    result(command->sequence, "sent");
                } catch (...) {
                    std::lock_guard lock(mutex_);
                    result(command->sequence, "unknown");
                    throw;
                }
            }
        } catch (const std::exception& error) {
            port.close(); parser.reset();
            {
                std::lock_guard lock(mutex_);
                serial_connected_ = false; sample_.reset();
                for (const auto& command : commands_) result(command.sequence, "rejected_offline");
                commands_.clear();
            }
            warn("serial", error);
            pause(stop_, config_.reconnect_interval_ms);
        }
    }
    std::lock_guard lock(mutex_);
    serial_connected_ = false;
}

void Stm32Service::mqtt_loop() noexcept {
    mqtt::MqttClient client(mqtt_, "-stm32", prefix_ + "/status");
    std::uint64_t published_generation = 0;
    auto next_status = Clock::now();
    while (!stop_) {
        try {
            if (!client.connected()) {
                client.connect(status_json(true));
                client.subscribe(prefix_ + "/command");
                mqtt_connected_ = true;
                next_status = Clock::now();
                published_generation = 0;
            }
            for (const auto& message : client.receive(20))
                if (message.topic == prefix_ + "/command") enqueue_command(message.payload, message.retained);
            if (Clock::now() >= next_status) {
                client.publish(status_json());
                next_status = Clock::now() + std::chrono::milliseconds(mqtt_.publish_interval_ms);
            }
            std::optional<Dht11Sample> sample;
            std::uint64_t generation = 0;
            std::optional<Event> outgoing;
            {
                std::lock_guard lock(mutex_);
                if (online_locked() && sample_generation_ != published_generation) {
                    sample = sample_; generation = sample_generation_;
                }
                if (!events_.empty()) outgoing = events_.front();
            }
            if (sample) {
                client.publish(prefix_ + "/telemetry", telemetry_json(*sample), false);
                published_generation = generation;
            }
            if (outgoing) {
                client.publish(outgoing->topic, outgoing->payload, false);
                std::lock_guard lock(mutex_);
                if (!events_.empty() && events_.front().id == outgoing->id) events_.pop_front();
            }
        } catch (const std::exception& error) {
            mqtt_connected_ = false;
            client.disconnect(status_json(true));
            {
                std::lock_guard lock(mutex_);
                for (const auto& command : commands_) result(command.sequence, "rejected_disconnected");
                commands_.clear();
            }
            warn("mqtt", error);
            pause(stop_, config_.reconnect_interval_ms);
        }
    }
    client.disconnect(status_json(true));
    mqtt_connected_ = false;
}

} // namespace rkmon::stm32
