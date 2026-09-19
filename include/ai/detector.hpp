#pragma once
#include "ai/config.hpp"
#include "media/frame.hpp"
#include <memory>
#include <string>
#include <vector>

namespace rkmon::ai {
struct Detection {
    int class_id{};
    std::string label;
    float confidence{};
    int left{}, top{}, right{}, bottom{};
};
struct DetectionResult {
    std::uint64_t sequence{};
    std::uint64_t source_generation{};
    unsigned worker_index{};
    bool source_dma{},input_dma{};
    std::chrono::steady_clock::time_point source_time{};
    int width{}, height{};
    double inference_ms{};
    double preprocess_ms{}, input_ms{}, npu_ms{}, postprocess_ms{};
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
std::unique_ptr<IObjectDetector> make_detector(const InferenceConfig& config, unsigned worker_index = 0);
}
