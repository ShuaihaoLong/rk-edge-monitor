#pragma once

#include "app/application.hpp"
#include "camera/video_source.hpp"
#include "video/interfaces/video_decoder.hpp"
#include "video/interfaces/video_publisher.hpp"
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
};
// 完整解析、校验后返回；相对路径以配置文件所在目录为基准。
RuntimeConfig load_config(const std::filesystem::path& path);
}
