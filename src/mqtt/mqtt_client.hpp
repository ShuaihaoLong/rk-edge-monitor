#pragma once

#include "mqtt/config.hpp"
#include <string>

namespace rkmon::mqtt {

// MQTT 3.1.1 的最小发布客户端，仅支持 retained QoS 0 状态消息。
// socket 只由状态服务线程访问；析构时不抛异常。
class MqttClient {
public:
    explicit MqttClient(Config config);
    ~MqttClient();
    void connect(const std::string& will_payload);
    void publish(const std::string& payload);
    void disconnect(const std::string& offline_payload) noexcept;
    [[nodiscard]] bool connected() const noexcept { return socket_ >= 0; }
    [[nodiscard]] const std::string& topic() const noexcept { return topic_; }
private:
    void send_packet(unsigned char header, const std::string& payload);
    void close_socket() noexcept;
    Config config_;
    std::string topic_;
    int socket_{-1};
};

} // namespace rkmon::mqtt
