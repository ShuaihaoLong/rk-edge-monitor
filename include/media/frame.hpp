#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rkmon::camera {

enum class PixelFormat { RGB888, BGR888, NV12, YUYV, MJPG };

struct VideoFrame {
    // 每次摄像头成功重新打开后递增，下游据此重建有状态的编解码管线。
    std::uint64_t source_generation{0};
    std::uint64_t sequence{0};
    int width{0};
    int height{0};
    PixelFormat format{PixelFormat::MJPG};
    std::size_t stride{0};
    // 应用取到帧的单调时钟时间；不冒充设备曝光时间。
    std::chrono::steady_clock::time_point timestamp{};
    // 自有只读存储，不借用已经归还驱动的 mmap 缓冲区。
    std::shared_ptr<const std::uint8_t[]> data;
    std::size_t size{0};
};

} // namespace rkmon::camera
