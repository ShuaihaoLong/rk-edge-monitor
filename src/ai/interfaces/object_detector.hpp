#pragma once
#include "camera/videoFrame.hpp"
#include <memory>
#include <string>
#include <vector>

namespace rkmon::ai {
struct InferenceConfig {
    std::string model_path, labels_path, result_path;
    unsigned fps{10};
};
struct Detection {
    int class_id{};
    std::string label;
    float confidence{};
    int left{}, top{}, right{}, bottom{};
};
struct DetectionResult {
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point source_time{};
    int width{}, height{};
    double inference_ms{};
    std::vector<Detection> objects;
};
// 单线程拥有检测器；close 必须在 detect 返回后执行，不与 NPU 调用并发。
class IObjectDetector {
public:
    virtual ~IObjectDetector() = default;
    virtual void open() = 0;
    virtual DetectionResult detect(const camera::VideoFrame&) = 0;
    virtual void close() noexcept = 0;
};
std::unique_ptr<IObjectDetector> make_detector(const InferenceConfig& config);
}
