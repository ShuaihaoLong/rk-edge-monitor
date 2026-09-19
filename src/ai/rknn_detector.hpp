#pragma once
#include "ai/detector.hpp"
namespace rkmon::ai {
class RknnDetector final : public IObjectDetector {
public:
    explicit RknnDetector(InferenceConfig, unsigned worker_index = 0);
    ~RknnDetector() override;
    void open() override;
    DetectionResult detect(const camera::VideoFrame&) override;
    void close() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
