#pragma once
#include "ai/interfaces/object_detector.hpp"
namespace rkmon::ai {
// 单写线程的只读 JSON 快照；原子替换避免 Nginx 读到半份结果。
class ResultWriter {
public:
    explicit ResultWriter(std::string path);
    void write(const std::string& status,const DetectionResult* result=nullptr);
private:
    std::string path_, session_;
    std::uint64_t revision_{0};
};
}
