#include "mqtt/device_status_service.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;

void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

void read_exact(int socket, void* destination, std::size_t size) {
    auto* data = static_cast<unsigned char*>(destination);
    while (size) {
        const auto count = recv(socket, data, size, 0);
        if (count <= 0) throw std::runtime_error("mock broker connection closed");
        data += count;
        size -= static_cast<std::size_t>(count);
    }
}

struct Packet { unsigned char header{}; std::string body; };

Packet read_packet(int socket) {
    Packet packet;
    read_exact(socket, &packet.header, 1);
    std::size_t remaining = 0, multiplier = 1;
    for (unsigned index = 0; index < 4; ++index) {
        unsigned char byte = 0;
        read_exact(socket, &byte, 1);
        remaining += (byte & 127) * multiplier;
        if (!(byte & 128)) break;
        multiplier *= 128;
    }
    packet.body.resize(remaining);
    if (remaining) read_exact(socket, packet.body.data(), remaining);
    return packet;
}

std::string publish_payload(const Packet& packet) {
    check(packet.body.size() >= 2, "short MQTT publish");
    const auto topic_size = (static_cast<unsigned char>(packet.body[0]) << 8) |
                            static_cast<unsigned char>(packet.body[1]);
    check(packet.body.size() >= topic_size + 2, "invalid MQTT topic length");
    check(packet.body.substr(2, topic_size) == "rkmon/devices/test-center/status", "wrong MQTT topic");
    return packet.body.substr(topic_size + 2);
}

template<class Predicate> void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, "MQTT test timed out");
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

int main() {
    int listener = -1;
    try {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        check(listener >= 0, "cannot create mock broker");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "cannot bind mock broker");
        check(listen(listener, 1) == 0, "cannot listen");
        socklen_t address_size = sizeof(address);
        check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0, "cannot read broker port");

        std::mutex mutex;
        std::vector<std::string> payloads;
        std::string will_packet;
        std::thread broker([&] {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) return;
            try {
                const auto connect = read_packet(client);
                check(connect.header == 0x10, "expected MQTT CONNECT");
                will_packet = connect.body;
                const unsigned char connack[]{0x20, 0x02, 0x00, 0x00};
                send(client, connack, sizeof(connack), MSG_NOSIGNAL);
                while (true) {
                    const auto packet = read_packet(client);
                    if (packet.header == 0xe0) break;
                    check(packet.header == 0x31, "status publish must be retained QoS 0");
                    std::lock_guard lock(mutex);
                    payloads.push_back(publish_payload(packet));
                }
            } catch (...) {}
            close(client);
        });

        rkmon::mqtt::Config config;
        config.broker_port = ntohs(address.sin_port);
        config.device_id = "test-center";
        config.role = "center";
        config.publish_interval_ms = 250;
        config.keepalive_seconds = 2;
        std::atomic<bool> camera{true};
        rkmon::mqtt::DeviceStatusService service(config, [&] { return camera.load(); });
        check(service.start(), "MQTT service failed to start");
        eventually([&] {
            std::lock_guard lock(mutex);
            return !payloads.empty() && payloads.back().find("\"camera_online\":true") != std::string::npos;
        });
        camera = false;
        eventually([&] {
            std::lock_guard lock(mutex);
            return payloads.back().find("\"camera_online\":false") != std::string::npos;
        });
        service.request_stop();
        service.join();
        broker.join();
        close(listener);
        listener = -1;

        check(will_packet.find("rkmon/devices/test-center/status") != std::string::npos,
              "CONNECT packet lacks status will topic");
        check(will_packet.find("\"online\":false") != std::string::npos,
              "CONNECT packet lacks offline last will");
        std::lock_guard lock(mutex);
        check(payloads.front().find("\"online\":true") != std::string::npos, "online state missing");
        check(payloads.front().find("\"role\":\"center\"") != std::string::npos, "role missing");
        check(payloads.front().find("\"ip\":") != std::string::npos, "IP missing");
        check(payloads.front().find("\"cpu_usage_percent\":") != std::string::npos, "CPU usage missing");
        check(payloads.front().find("\"cpu_temperature_c\":") != std::string::npos, "temperature missing");
        check(payloads.front().find("\"heartbeat_interval_ms\":250") != std::string::npos,
              "heartbeat interval missing");
        check(payloads.back().find("\"online\":false") != std::string::npos, "graceful offline state missing");
        std::cout << "MQTT connect, LWT, retained status and camera state tests passed\n";
    } catch (const std::exception& error) {
        if (listener >= 0) close(listener);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
