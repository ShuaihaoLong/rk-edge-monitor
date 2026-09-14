#include "device_status_service.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <iomanip>
#include <net/if.h>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace rkmon::mqtt {
namespace {

std::string local_ipv4() {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return {};
    struct Guard { ifaddrs* value; ~Guard() { freeifaddrs(value); } } guard{interfaces};
    std::string fallback;
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_LOOPBACK) || !(item->ifa_flags & IFF_UP)) continue;
        char address[INET_ADDRSTRLEN]{};
        const auto* ipv4 = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        if (!inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address))) continue;
        if (std::string(item->ifa_name).rfind("eth", 0) == 0 ||
            std::string(item->ifa_name).rfind("en", 0) == 0) return address;
        if (fallback.empty()) fallback = address;
    }
    return fallback;
}

std::optional<double> cpu_temperature() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/thermal", error)) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
        std::ifstream input(entry.path() / "temp");
        double value = 0;
        if (input >> value) return value / 1000.0;
    }
    return std::nullopt;
}

bool cpu_times(std::uint64_t& total, std::uint64_t& idle) {
    std::ifstream input("/proc/stat");
    std::string name;
    std::uint64_t user=0,nice=0,system=0,idle_value=0,iowait=0,irq=0,softirq=0,steal=0;
    if (!(input >> name >> user >> nice >> system >> idle_value >> iowait >> irq >> softirq >> steal) || name != "cpu")
        return false;
    idle = idle_value + iowait;
    total = user + nice + system + idle_value + iowait + irq + softirq + steal;
    return true;
}

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') out << '\\' << character;
        else if (character < 32) out << "\\u" << std::hex << std::setw(4)
                                     << std::setfill('0') << unsigned(character) << std::dec;
        else out << character;
    }
    return out.str() + '"';
}

} // namespace

DeviceStatusService::DeviceStatusService(Config config, CameraStatus camera_status,
                                         std::shared_ptr<spdlog::logger> logger)
    : config_(std::move(config)), camera_status_(std::move(camera_status)),
      logger_(std::move(logger)), client_(config_) {
    if (config_.device_id.empty() || config_.device_id.size() > 64 ||
        (config_.role != "center" && config_.role != "edge") ||
        config_.publish_interval_ms < 250 || config_.publish_interval_ms > 60000 ||
        config_.keepalive_seconds < 2 || config_.publish_interval_ms >= config_.keepalive_seconds * 1000)
        throw std::invalid_argument("invalid MQTT status configuration");
}

DeviceStatusService::~DeviceStatusService() { request_stop(); join(); }

bool DeviceStatusService::start() {
    if (running_) return true;
    if (worker_.joinable()) throw std::logic_error("join MQTT service before restarting");
    stop_ = false;
    connected_ = false;
    running_ = true;
    cpu_times(previous_total_, previous_idle_);
    try {
        worker_ = std::thread(&DeviceStatusService::run, this);
        return true;
    } catch (...) {
        running_ = false;
        throw;
    }
}

void DeviceStatusService::request_stop() noexcept { stop_ = true; }

void DeviceStatusService::join() noexcept {
    if (worker_.joinable()) worker_.join();
    running_ = false;
}

core::HealthSnapshot DeviceStatusService::health() const {
    std::lock_guard lock(mutex_);
    return {connected_ ? core::ServiceState::running : core::ServiceState::degraded, detail_};
}

std::string DeviceStatusService::status_json(bool online) {
    std::uint64_t total = 0, idle = 0;
    std::optional<double> usage;
    if (cpu_times(total, idle) && previous_total_ && total > previous_total_) {
        const auto total_delta = total - previous_total_;
        const auto idle_delta = idle - previous_idle_;
        usage = 100.0 * static_cast<double>(total_delta - std::min(idle_delta, total_delta)) / total_delta;
    }
    previous_total_ = total;
    previous_idle_ = idle;
    const auto temperature = cpu_temperature();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream out;
    out << "{\"schema\":1,\"device_id\":" << json_string(config_.device_id)
        << ",\"role\":" << json_string(config_.role) << ",\"ip\":" << json_string(local_ipv4())
        << ",\"online\":" << (online ? "true" : "false")
        << ",\"camera_online\":" << (online && camera_status_ && camera_status_() ? "true" : "false")
        << ",\"heartbeat_interval_ms\":" << config_.publish_interval_ms
        << ",\"cpu_usage_percent\":";
    if (usage) out << std::fixed << std::setprecision(1) << *usage; else out << "null";
    out << ",\"cpu_temperature_c\":";
    if (temperature) out << std::fixed << std::setprecision(1) << *temperature; else out << "null";
    out << ",\"updated_at_ms\":" << now << '}';
    return out.str();
}

void DeviceStatusService::run() noexcept {
    auto retry_at = std::chrono::steady_clock::now();
    auto next_publish = retry_at;
    const auto offline = status_json(false);
    while (!stop_) {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (!client_.connected() && now >= retry_at) {
                client_.connect(offline);
                connected_ = true;
                {
                    std::lock_guard lock(mutex_);
                    detail_.clear();
                }
                next_publish = now;
                try { if (logger_) logger_->info("[mqtt] connected, topic={}", client_.topic()); } catch (...) {}
            }
            if (client_.connected() && now >= next_publish) {
                client_.publish(status_json(true));
                next_publish = now + std::chrono::milliseconds(config_.publish_interval_ms);
            }
        } catch (const std::exception& error) {
            client_.disconnect(offline);
            connected_ = false;
            {
                std::lock_guard lock(mutex_);
                detail_ = error.what();
            }
            retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            try { if (logger_) logger_->warn("[mqtt] {}; retry in 2s", error.what()); } catch (...) {}
        }
        for (unsigned i = 0; i < 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client_.disconnect(status_json(false));
    connected_ = false;
    running_ = false;
}

} // namespace rkmon::mqtt
