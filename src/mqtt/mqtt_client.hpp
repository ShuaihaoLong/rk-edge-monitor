#pragma once

#include "mqtt/config.hpp"
#include <string>
#include <chrono>
#include <vector>

namespace rkmon::mqtt {

// MQTT 3.1.1 QoS 0 发布/订阅客户端；每个实例的 socket 仅由一个线程访问。
// 订阅客户端须持续调用 receive，处理报文、订阅确认和保活。
class MqttClient {
public:
    struct Message { std::string topic, payload; bool retained{}; };
    explicit MqttClient(Config config, std::string client_suffix = {}, std::string status_topic = {});
    ~MqttClient();
    void connect(const std::string& will_payload);
    void publish(const std::string& payload);
    void publish(const std::string& topic, const std::string& payload, bool retained);
    void subscribe(const std::string& topic);
    std::vector<Message> receive(int timeout_ms);
    void disconnect(const std::string& offline_payload) noexcept;
    [[nodiscard]] bool connected() const noexcept { return socket_ >= 0; }
    [[nodiscard]] const std::string& topic() const noexcept { return topic_; }
private:
    void send_packet(unsigned char header, const std::string& payload);
    void close_socket() noexcept;
    Config config_;
    std::string topic_;
    std::string client_id_, receive_buffer_;
    bool subscription_pending_{false}, ping_pending_{false};
    std::chrono::steady_clock::time_point subscription_at_{}, ping_at_{};
    int socket_{-1};
};

} // namespace rkmon::mqtt
