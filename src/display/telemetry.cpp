#include "telemetry.hpp"
#include "mqtt/mqtt_client.hpp"
#include <json-c/json.h>
#include <cmath>
#include <memory>
#include <iostream>

namespace rkmon::display {
namespace {
bool number(json_object* obj, const char* key, double& value) {
    json_object* field = nullptr;
    if (!json_object_object_get_ex(obj, key, &field) ||
        (!json_object_is_type(field, json_type_double) &&
         !json_object_is_type(field, json_type_int)))
        return false;
    value = json_object_get_double(field);
    return std::isfinite(value);
}
}

void SensorState::accept(const std::string& kind, const std::string& payload, bool retained) {
    if (payload.size() > 4096)
        return;
    std::unique_ptr<json_object, decltype(&json_object_put)> obj(
        json_tokener_parse(payload.c_str()), json_object_put);
    if (!obj || !json_object_is_type(obj.get(), json_type_object))
        return;
    double schema = 0;
    if (!number(obj.get(), "schema", schema) || schema != 1)
        return;
    if (kind == "status") {
        json_object* value = nullptr;
        if (!json_object_object_get_ex(obj.get(), "online", &value) ||
            !json_object_is_type(value, json_type_boolean))
            return;
        connected_ = true;
        online_ = json_object_get_boolean(value);
        if (!online_)
            has_sample_ = false;
    } else if (kind == "telemetry" && !retained) {
        double temp, humi, source_ms;
        if (!number(obj.get(), "temperature_c", temp) || temp < 0 || temp > 50 ||
            !number(obj.get(), "humidity_percent", humi) || humi < 0 || humi > 100 ||
            !number(obj.get(), "received_at_ms", source_ms))
            return;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms - source_ms > stale_ms_ || source_ms - now_ms > 30000)
            return;
        connected_ = true;
        has_sample_ = true;
        temperature_ = temp;
        humidity_ = humi;
        received_ = std::chrono::steady_clock::now();
    }
}

void SensorState::disconnected() {
    connected_ = false;
    online_ = false;
    has_sample_ = false;
}

SensorSnapshot SensorState::snapshot() const {
    const bool fresh = has_sample_ && std::chrono::steady_clock::now() - received_ <
                                          std::chrono::milliseconds(stale_ms_);
    const bool valid = connected_ && online_ && fresh;
    return {valid, temperature_, humidity_,
            valid         ? "STM32 在线"
            : !connected_ ? "传感器连接中"
            : !online_    ? "STM32 离线"
                          : "等待新数据 / 已过期"};
}

Telemetry::Telemetry(mqtt::Config config, unsigned stale_ms)
    : config_(std::move(config)), state_(stale_ms), thread_(&Telemetry::run, this) {}

Telemetry::~Telemetry() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

SensorSnapshot Telemetry::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_.snapshot();
}

void Telemetry::run() noexcept {
    const auto prefix = config_.topic_prefix + "/" + config_.device_id + "/stm32/";
    mqtt::MqttClient client(config_, "-display",
                            config_.topic_prefix + "/" + config_.device_id + "/display/status");
    while (!stop_) {
        try {
            if (!client.connected()) {
                client.connect("{\"online\":false}");
                client.subscribe(prefix + "+");
            }
            for (const auto& message : client.receive(50)) {
                if (message.topic.compare(0, prefix.size(), prefix))
                    continue;
                std::lock_guard lock(mutex_);
                state_.accept(message.topic.substr(prefix.size()), message.payload,
                              message.retained);
            }
        } catch (const std::exception& error) {
            std::cerr << "[display/mqtt] " << error.what() << '\n';
            client.disconnect("{\"online\":false}");
            {
                std::lock_guard lock(mutex_);
                state_.disconnected();
            }
            for (int i = 0; i < 100 && !stop_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    client.disconnect("{\"online\":false}");
}
}
