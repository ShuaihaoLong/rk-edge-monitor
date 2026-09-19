#include "app/config.hpp"

#include <charconv>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace rkmon::app {
namespace {
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
struct Entry { std::string value; std::size_t line; };
class Ini {
public:
    explicit Ini(const std::filesystem::path& path) : path_(path.string()) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot open configuration: " + path_);
        std::string line, section;
        std::size_t number = 0, bytes = 0;
        std::set<std::string> sections;
        while (std::getline(file, line)) {
            ++number;
            bytes += line.size();
            if (bytes > 1024 * 1024) error(number, "configuration exceeds 1 MiB");
            if (number == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
            line = trim(line);
            if (line.empty() || line.front() == '#' || line.front() == ';') continue;
            if (line.front() == '[') {
                if (line.back() != ']') error(number, "malformed section");
                section = trim(line.substr(1, line.size() - 2));
                if (section != "app" && section != "logging" && section != "camera" && section != "video" &&
                    section != "stream" && section != "ai" && section != "mqtt" && section != "stm32")
                    error(number, "unknown section: " + section);
                if (!sections.insert(section).second) error(number, "duplicate section: " + section);
                continue;
            }
            const auto separator = line.find('=');
            if (section.empty() || separator == std::string::npos) error(number, "expected section and key=value");
            const auto key = trim(line.substr(0, separator));
            if (key.empty()) error(number, "empty key");
            if (!entries_.emplace(section + "." + key, Entry{trim(line.substr(separator + 1)), number}).second)
                error(number, "duplicate key: " + section + "." + key);
        }
        if (file.bad()) throw std::runtime_error("cannot read configuration: " + path_);
    }
    std::string take(const std::string& key, const std::string& fallback) {
        auto entry = entries_.find(key);
        if (entry == entries_.end()) return fallback;
        auto result = entry->second.value;
        entries_.erase(entry);
        return result;
    }
    unsigned integer(const std::string& key, unsigned fallback, unsigned minimum, unsigned maximum) {
        const auto text = take(key, std::to_string(fallback));
        unsigned value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
            throw std::runtime_error(path_ + ": " + key + " must be an integer in [" +
                                     std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
        return value;
    }
    bool boolean(const std::string& key, bool fallback) {
        const auto text = take(key, fallback ? "true" : "false");
        if (text == "true") return true;
        if (text == "false") return false;
        throw std::runtime_error(path_ + ": " + key + " must be true or false");
    }
    void finish() const {
        if (!entries_.empty()) error(entries_.begin()->second.line, "unknown key: " + entries_.begin()->first);
    }
private:
    [[noreturn]] void error(std::size_t line, const std::string& message) const {
        throw std::runtime_error(path_ + ":" + std::to_string(line) + ": " + message);
    }
    std::string path_;
    std::map<std::string, Entry> entries_;
};
}

RuntimeConfig load_config(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    Ini ini(absolute);
    RuntimeConfig config;
    auto resolve = [&](const std::string& value) {
        return (absolute.parent_path() / std::filesystem::path(value)).lexically_normal().string();
    };
    config.application.control_mailbox_capacity = ini.integer("app.control_mailbox_capacity", 64, 1, 65536);
    auto& log = config.application.log;
    log.console = ini.boolean("logging.console", true);
    log.file_path = ini.take("logging.file", "../logs/rkmon.log");
    if (!log.file_path.empty()) log.file_path = resolve(log.file_path);
    log.max_file_size = ini.integer("logging.max_file_size", 5 * 1024 * 1024, 1, 1024 * 1024 * 1024);
    log.rotated_files = ini.integer("logging.rotated_files", 3, 0, 100);
    const auto level = ini.take("logging.level", "info");
    const std::map<std::string, spdlog::level::level_enum> levels{
        {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug}, {"info", spdlog::level::info},
        {"warn", spdlog::level::warn}, {"error", spdlog::level::err}, {"critical", spdlog::level::critical},
        {"off", spdlog::level::off}};
    const auto selected = levels.find(level);
    if (selected == levels.end()) throw std::runtime_error(absolute.string() + ": invalid logging.level: " + level);
    log.level = selected->second;
    if (!log.console && log.file_path.empty())
        throw std::runtime_error(absolute.string() + ": logging requires console or file output");

    const bool enabled = ini.boolean("camera.enabled", false);
    CameraSettings camera;
    auto& capture = camera.capture;
    capture.device = ini.take("camera.device", "");
    if (enabled && capture.device.empty())
        throw std::runtime_error(absolute.string() + ": camera.device is required when camera.enabled=true");
    if (!capture.device.empty()) capture.device = resolve(capture.device);
    capture.width = static_cast<int>(ini.integer("camera.width", 1920, 1, 16384));
    capture.height = static_cast<int>(ini.integer("camera.height", 1080, 1, 16384));
    capture.fps = ini.integer("camera.fps", 30, 1, 1000);
    capture.buffer_count = ini.integer("camera.buffer_count", 4, 2, 64);
    capture.poll_timeout_ms = static_cast<int>(ini.integer("camera.poll_timeout_ms", 1000, 1, 60000));
    capture.max_consecutive_timeouts = ini.integer("camera.max_consecutive_timeouts", 5, 1, 1000);
    capture.reconnect_interval_ms = ini.integer("camera.reconnect_interval_ms", 2000, 100, 60000);
    camera.queue_capacity = ini.integer("camera.queue_capacity", 4, 1, 64);
    const auto format = ini.take("camera.format", "MJPG");
    if (format == "MJPG") capture.format = camera::PixelFormat::MJPG;
    else if (format == "YUYV") capture.format = camera::PixelFormat::YUYV;
    else throw std::runtime_error(absolute.string() + ": camera.format must be MJPG or YUYV");
    if (enabled) config.camera = std::move(camera);
    const bool decode_enabled = ini.boolean("video.enabled", false);
    video::DecodeConfig decode;
    decode.timeout_ms = ini.integer("video.timeout_ms", 2000, 1, 60000);
    decode.queue_capacity = ini.integer("video.queue_capacity", 4, 1, 64);
    if (decode_enabled) {
        if (!config.camera || config.camera->capture.format != camera::PixelFormat::MJPG) {
            throw std::runtime_error(absolute.string() + ": video decoding requires an enabled MJPG camera");
        }
        config.video = decode;
    }
    const bool stream_enabled = ini.boolean("stream.enabled", false);
    video::StreamConfig stream;
    stream.url = ini.take("stream.url", "rtsp://127.0.0.1:8554/camera");
    stream.bitrate = ini.integer("stream.bitrate", 4000000, 100000, 100000000);
    stream.gop = ini.integer("stream.gop", 30, 1, 1000);
    stream.timeout_ms = ini.integer("stream.timeout_ms", 5000, 100, 60000);
    stream.osd_enabled = ini.boolean("stream.osd_enabled", true);
    stream.osd_timezone = ini.take("stream.osd_timezone", "Asia/Shanghai");
    if (stream.osd_timezone != "Asia/Shanghai" && stream.osd_timezone != "UTC" && stream.osd_timezone != "local")
        throw std::runtime_error(absolute.string() + ": stream.osd_timezone must be Asia/Shanghai, UTC or local");
    if (stream_enabled) {
        if (!config.video) {
            throw std::runtime_error(absolute.string() + ": streaming requires video.enabled=true");
        }
        if (stream.url.rfind("rtsp://", 0) != 0 || stream.url.size() <= 7 ||
            stream.url.find_first_of(" \t\r\n") != std::string::npos) {
            throw std::runtime_error(absolute.string() + ": stream.url must be an RTSP URL");
        }
        stream.fps = config.camera->capture.fps;
        config.stream = stream;
    }
    const bool ai_enabled = ini.boolean("ai.enabled", false);
    ai::InferenceConfig ai;
    ai.model_path = resolve(ini.take("ai.model", "../models/yolov8n.rknn"));
    ai.labels_path = resolve(ini.take("ai.labels", "../models/coco_80_labels_list.txt"));
    ai.result_path = resolve(ini.take("ai.result", "../run/detections.json"));
    ai.fps = ini.integer("ai.fps", 10, 1, 60);
    if (ai_enabled) {
        if (!config.video) throw std::runtime_error(absolute.string() + ": AI requires video.enabled=true");
        config.ai = ai;
    }
    const bool mqtt_enabled = ini.boolean("mqtt.enabled", false);
    mqtt::Config mqtt;
    mqtt.broker_host = ini.take("mqtt.broker_host", "127.0.0.1");
    mqtt.broker_port = ini.integer("mqtt.broker_port", 1883, 1, 65535);
    mqtt.device_id = ini.take("mqtt.device_id", "");
    mqtt.role = ini.take("mqtt.role", "edge");
    mqtt.topic_prefix = ini.take("mqtt.topic_prefix", "rkmon/devices");
    mqtt.publish_interval_ms = ini.integer("mqtt.publish_interval_ms", 2000, 250, 60000);
    mqtt.keepalive_seconds = ini.integer("mqtt.keepalive_seconds", 10, 2, 65535);
    if (mqtt_enabled) {
        const auto valid_id = !mqtt.device_id.empty() && mqtt.device_id.size() <= 64 &&
            std::all_of(mqtt.device_id.begin(), mqtt.device_id.end(), [](unsigned char character) {
                return std::isalnum(character) || character == '-' || character == '_';
            });
        if (!valid_id) throw std::runtime_error(absolute.string() + ": mqtt.device_id must use letters, digits, '-' or '_'");
        if (mqtt.broker_host.empty() || mqtt.broker_host.find_first_of(" \t\r\n") != std::string::npos)
            throw std::runtime_error(absolute.string() + ": invalid mqtt.broker_host");
        if (mqtt.role != "center" && mqtt.role != "edge")
            throw std::runtime_error(absolute.string() + ": mqtt.role must be center or edge");
        if (mqtt.topic_prefix != "rkmon/devices")
            throw std::runtime_error(absolute.string() + ": mqtt.topic_prefix must be rkmon/devices");
        if (mqtt.publish_interval_ms >= mqtt.keepalive_seconds * 1000)
            throw std::runtime_error(absolute.string() + ": MQTT publish interval must be shorter than keepalive");
        config.mqtt = mqtt;
    }
    const bool stm32_enabled = ini.boolean("stm32.enabled", false);
    stm32::Config stm32;
    stm32.device = ini.take("stm32.device", "/dev/ttyS9");
    if (!stm32.device.empty()) stm32.device = resolve(stm32.device);
    stm32.baud_rate = ini.integer("stm32.baud_rate", 115200, 9600, 230400);
    stm32.frame_timeout_ms = ini.integer("stm32.frame_timeout_ms", 200, 20, 5000);
    stm32.stale_timeout_ms = ini.integer("stm32.stale_timeout_ms", 5000, 500, 60000);
    stm32.reconnect_interval_ms = ini.integer("stm32.reconnect_interval_ms", 2000, 100, 60000);
    if (stm32.baud_rate != 9600 && stm32.baud_rate != 19200 && stm32.baud_rate != 38400 &&
        stm32.baud_rate != 57600 && stm32.baud_rate != 115200 && stm32.baud_rate != 230400)
        throw std::runtime_error(absolute.string() + ": unsupported stm32.baud_rate");
    if (stm32_enabled) {
        if (!config.mqtt) throw std::runtime_error(absolute.string() + ": STM32 requires mqtt.enabled=true");
        if (stm32.device.empty()) throw std::runtime_error(absolute.string() + ": stm32.device is required");
        config.stm32 = stm32;
    }
    ini.finish();
    return config;
}
}
