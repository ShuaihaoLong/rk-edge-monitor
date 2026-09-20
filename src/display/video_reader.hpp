#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace rkmon::display {
struct VideoImage {
    std::vector<std::uint8_t> pixels;
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point received;
};
// 工作线程持有 GStreamer 对象；交接时复制一帧 RGB，不让 UI 持有硬解缓冲区。
class VideoReader {
public:
    explicit VideoReader(std::string url);
    ~VideoReader();
    bool take(VideoImage& frame);
    std::string status() const;
private:
    void run() noexcept;
    std::string url_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    VideoImage latest_;
    std::string status_{"正在连接摄像头"};
    std::thread thread_;
};
}
