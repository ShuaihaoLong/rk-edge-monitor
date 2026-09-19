#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rkmon::stm32 {

constexpr std::uint8_t msg_ack = 0x00;
constexpr std::uint8_t msg_dht11_data = 0x10;
constexpr std::uint8_t msg_cmd_ctrl = 0x20;
constexpr std::size_t max_payload = 128;

struct Frame {
    std::uint8_t type{};
    std::uint32_t sequence{};
    std::vector<std::uint8_t> payload;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;
std::vector<std::uint8_t> encode(const Frame& frame);

// 每个串口连接独占一个解析器；断线后 reset，防止跨连接拼接帧。
class FrameParser {
public:
    using Clock = std::chrono::steady_clock;
    explicit FrameParser(std::chrono::milliseconds timeout = std::chrono::milliseconds(200));
    std::vector<Frame> feed(const std::uint8_t* data, std::size_t size,
                            Clock::time_point now = Clock::now());
    void reset() noexcept;
private:
    void parse(std::vector<Frame>& frames, Clock::time_point now);
    std::vector<std::uint8_t> buffer_;
    std::chrono::milliseconds timeout_;
    Clock::time_point candidate_since_{};
    bool candidate_{false};
};

} // namespace rkmon::stm32
