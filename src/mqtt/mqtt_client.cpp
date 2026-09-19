#include "mqtt_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace rkmon::mqtt {
namespace {

void append_string(std::string& target, const std::string& value) {
    if (value.size() > 65535) throw std::invalid_argument("MQTT string is too long");
    target.push_back(static_cast<char>(value.size() >> 8));
    target.push_back(static_cast<char>(value.size()));
    target += value;
}

std::string encode_length(std::size_t length) {
    if (length > 268435455) throw std::invalid_argument("MQTT packet is too large");
    std::string bytes;
    do {
        unsigned char value = length % 128;
        length /= 128;
        if (length) value |= 0x80;
        bytes.push_back(static_cast<char>(value));
    } while (length);
    return bytes;
}

void send_all(int socket, const char* data, std::size_t size) {
    while (size) {
        const auto sent = ::send(socket, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) throw std::runtime_error("MQTT send failed: " + std::string(std::strerror(errno)));
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

} // namespace

MqttClient::MqttClient(Config config, std::string client_suffix, std::string status_topic)
    : config_(std::move(config)), client_id_("rkmon-" + config_.device_id + client_suffix) {
    topic_ = status_topic.empty() ? config_.topic_prefix + "/" + config_.device_id + "/status" : std::move(status_topic);
}

MqttClient::~MqttClient() { close_socket(); }

void MqttClient::connect(const std::string& will_payload) {
    close_socket();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto port = std::to_string(config_.broker_port);
    if (getaddrinfo(config_.broker_host.c_str(), port.c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("cannot resolve MQTT broker");
    struct AddressGuard { addrinfo* value; ~AddressGuard() { freeaddrinfo(value); } } guard{addresses};
    for (auto* address = addresses; address; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) continue;
        const int flags = fcntl(candidate, F_GETFL, 0);
        fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
        const int result = ::connect(candidate, address->ai_addr, address->ai_addrlen);
        pollfd descriptor{candidate, POLLOUT, 0};
        if ((result == 0 || (errno == EINPROGRESS && poll(&descriptor, 1, 2000) == 1))) {
            int error = 0;
            socklen_t size = sizeof(error);
            getsockopt(candidate, SOL_SOCKET, SO_ERROR, &error, &size);
            if (error == 0) {
                fcntl(candidate, F_SETFL, flags);
                timeval timeout{2, 0};
                setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                socket_ = candidate;
                break;
            }
        }
        ::close(candidate);
    }
    if (socket_ < 0) throw std::runtime_error("cannot connect to MQTT broker");

    try {
        std::string body;
        append_string(body, "MQTT");
        body.push_back(4); // MQTT 3.1.1
        body.push_back(0x26); // clean session + will flag + retained will, QoS 0
        body.push_back(static_cast<char>(config_.keepalive_seconds >> 8));
        body.push_back(static_cast<char>(config_.keepalive_seconds));
        append_string(body, client_id_);
        append_string(body, topic_);
        append_string(body, will_payload);
        send_packet(0x10, body);
        unsigned char reply[4]{};
        std::size_t received = 0;
        while (received < sizeof(reply)) {
            const auto count = recv(socket_, reply + received, sizeof(reply) - received, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("MQTT CONNACK timeout");
            received += static_cast<std::size_t>(count);
        }
        if (reply[0] != 0x20 || reply[1] != 2 || reply[2] != 0 || reply[3] != 0)
            throw std::runtime_error("MQTT broker rejected connection");
        ping_at_ = std::chrono::steady_clock::now();
    } catch (...) {
        close_socket();
        throw;
    }
}

void MqttClient::publish(const std::string& payload) {
    publish(topic_, payload, true);
}

void MqttClient::publish(const std::string& topic, const std::string& payload, bool retained) {
    if (socket_ < 0) throw std::logic_error("MQTT client is disconnected");
    std::string body;
    append_string(body, topic);
    body += payload;
    send_packet(retained ? 0x31 : 0x30, body);
}

void MqttClient::subscribe(const std::string& topic) {
    if (socket_ < 0 || subscription_pending_) throw std::logic_error("MQTT subscription unavailable");
    std::string body("\x00\x01", 2);
    append_string(body, topic);
    body.push_back(0); // QoS 0，不在重连后重放控制消息。
    send_packet(0x82, body);
    subscription_pending_ = true;
    subscription_at_ = std::chrono::steady_clock::now();
}

std::vector<MqttClient::Message> MqttClient::receive(int timeout_ms) {
    if (socket_ < 0) throw std::logic_error("MQTT client is disconnected");
    const auto now = std::chrono::steady_clock::now();
    if (subscription_pending_ && now - subscription_at_ > std::chrono::seconds(2))
        throw std::runtime_error("MQTT SUBACK timeout");
    if (ping_pending_ && now - ping_at_ > std::chrono::seconds(config_.keepalive_seconds))
        throw std::runtime_error("MQTT PINGRESP timeout");
    if (!ping_pending_ && now - ping_at_ >= std::chrono::seconds(config_.keepalive_seconds / 2)) {
        send_packet(0xc0, {});
        ping_pending_ = true;
        ping_at_ = now;
    }
    pollfd descriptor{socket_, POLLIN, 0};
    const auto ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0 && errno != EINTR) throw std::runtime_error("MQTT receive poll failed");
    if (ready > 0) {
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("MQTT connection closed");
        char bytes[4096];
        const auto count = recv(socket_, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            throw std::runtime_error("MQTT receive failed");
        if (count > 0) receive_buffer_.append(bytes, static_cast<std::size_t>(count));
    }
    std::vector<Message> messages;
    while (receive_buffer_.size() >= 2) {
        std::size_t length = 0, multiplier = 1, offset = 1;
        bool complete = false;
        for (unsigned i = 0; i < 4 && offset < receive_buffer_.size(); ++i) {
            const auto byte = static_cast<unsigned char>(receive_buffer_[offset++]);
            length += (byte & 127) * multiplier;
            if (!(byte & 128)) { complete = true; break; }
            if (i == 3) throw std::runtime_error("invalid MQTT remaining length");
            multiplier *= 128;
        }
        if (length > 4096) throw std::runtime_error("MQTT incoming packet exceeds 4096 bytes");
        if (!complete || receive_buffer_.size() < offset + length) break;
        const auto header = static_cast<unsigned char>(receive_buffer_[0]);
        const auto body = receive_buffer_.substr(offset, length);
        receive_buffer_.erase(0, offset + length);
        if ((header & 0xf0) == 0x30) {
            if ((header & 0x0e) != 0 || body.size() < 2)
                throw std::runtime_error("expected MQTT QoS 0 PUBLISH");
            const auto topic_size = (static_cast<unsigned char>(body[0]) << 8) |
                                     static_cast<unsigned char>(body[1]);
            if (!topic_size || body.size() < 2u + topic_size)
                throw std::runtime_error("invalid MQTT PUBLISH topic");
            messages.push_back({body.substr(2, topic_size), body.substr(2 + topic_size), (header & 1) != 0});
        } else if (header == 0x90) {
            if (!subscription_pending_ || body != std::string("\x00\x01\x00", 3))
                throw std::runtime_error("MQTT subscription rejected");
            subscription_pending_ = false;
        } else if (header == 0xd0 && body.empty() && ping_pending_) {
            ping_pending_ = false;
        } else {
            throw std::runtime_error("unexpected MQTT packet");
        }
    }
    return messages;
}

void MqttClient::disconnect(const std::string& offline_payload) noexcept {
    if (socket_ < 0) return;
    try {
        publish(offline_payload);
        send_packet(0xe0, {});
    } catch (...) {}
    close_socket();
}

void MqttClient::send_packet(unsigned char header, const std::string& payload) {
    const auto length = encode_length(payload.size());
    std::vector<char> packet;
    packet.reserve(1 + length.size() + payload.size());
    packet.push_back(static_cast<char>(header));
    packet.insert(packet.end(), length.begin(), length.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    send_all(socket_, packet.data(), packet.size());
}

void MqttClient::close_socket() noexcept {
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
    receive_buffer_.clear();
    subscription_pending_ = false;
    ping_pending_ = false;
}

} // namespace rkmon::mqtt
