#pragma once

#include "app/application.hpp"
#include "camera/video_source.hpp"
#include "media/decoder.hpp"
#include "media/publisher.hpp"
#include "ai/detector.hpp"
#include "mqtt/config.hpp"
#include "stm32/config.hpp"
#include <filesystem>
#include <optional>

namespace rkmon::app {
struct CameraSettings {
    camera::CaptureConfig capture;
    std::size_t queue_capacity{4};
};
struct RuntimeConfig {
    Application::Options application;
    std::optional<CameraSettings> camera;
    std::optional<video::DecodeConfig> video;
    std::optional<video::StreamConfig> stream;
    std::optional<ai::InferenceConfig> ai;
    std::optional<mqtt::Config> mqtt;
    std::optional<stm32::Config> stm32;
};
// 完整解析、校验后返回；相对路径以配置文件所在目录为基准。
RuntimeConfig load_config(const std::filesystem::path& path);
}
