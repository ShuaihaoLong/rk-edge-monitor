#pragma once
#include "video_reader.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rkmon::display {
class AdReader {
public:
    explicit AdReader(std::string root);
    ~AdReader();
    bool take(VideoImage& frame);
    bool advertising() const;
private:
    void run() noexcept;
    std::vector<std::string> playlist() const;
    std::string mode() const;
    std::string root_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> advertising_{false};
    mutable std::mutex mutex_;
    VideoImage latest_;
    std::thread thread_;
};
}
