#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace rkmon::video {
class FrameRate {
public:
    double observe(std::chrono::steady_clock::time_point timestamp) {
        if (!start_ || timestamp <= previous_) {
            start_ = timestamp;
            frames_ = 0;
            rate_ = 0;
        } else {
            ++frames_;
            const auto seconds = std::chrono::duration<double>(timestamp - *start_).count();
            if (seconds >= 1.0) {
                rate_ = static_cast<double>(frames_) / seconds;
                start_ = timestamp;
                frames_ = 0;
            }
        }
        previous_ = timestamp;
        return rate_;
    }
private:
    std::optional<std::chrono::steady_clock::time_point> start_;
    std::chrono::steady_clock::time_point previous_{};
    std::uint64_t frames_{0};
    double rate_{0};
};
} // namespace rkmon::video
