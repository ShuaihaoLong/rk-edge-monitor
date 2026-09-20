#pragma once
#include "ai/detector.hpp"

namespace rkmon::ai {
// 单写线程的只读 JSON 快照；原子替换避免 Nginx 读到半份结果。
class ResultWriter {
public:
    explicit ResultWriter(std::string path, std::string event_socket = {});
    ~ResultWriter();
    ResultWriter(const ResultWriter&) = delete;
    ResultWriter& operator=(const ResultWriter&) = delete;
    void write(const std::string& status, const DetectionResult* result = nullptr);

private:
    std::string event_socket_;
    int event_fd_{-1};
    std::string path_, session_;
    std::uint64_t revision_{0};
};
}
