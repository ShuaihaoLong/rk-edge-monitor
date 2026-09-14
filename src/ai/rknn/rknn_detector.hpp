#pragma once
#include "ai/interfaces/object_detector.hpp"
namespace rkmon::ai {
class RknnDetector final : public IObjectDetector {
public:
    explicit RknnDetector(InferenceConfig);
    ~RknnDetector() override;
    void open() override;
    DetectionResult detect(const camera::VideoFrame&) override;
    void close() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
