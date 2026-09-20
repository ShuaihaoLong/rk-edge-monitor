#include "stm32/protocol.hpp"

#include <stdexcept>

namespace rkmon::stm32 {
namespace {
std::uint32_t read_le(const std::uint8_t* bytes, unsigned count) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < count; ++i)
        result |= std::uint32_t(bytes[i]) << (i * 8);
    return result;
}

void append_le(std::vector<std::uint8_t>& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

std::vector<std::uint8_t> encode(const Frame& frame) {
    if (frame.payload.size() > max_payload)
        throw std::invalid_argument("STM32 payload exceeds 128 bytes");
    std::vector<std::uint8_t> bytes{0xaa, 0x55, 1, frame.type};
    append_le(bytes, frame.sequence, 4);
    append_le(bytes, static_cast<std::uint32_t>(frame.payload.size()), 2);
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    const auto crc = crc32(bytes.data() + 2, bytes.size() - 2);
    append_le(bytes, crc, 4);
    return bytes;
}

FrameParser::FrameParser(std::chrono::milliseconds timeout) : timeout_(timeout) {
    if (timeout.count() <= 0)
        throw std::invalid_argument("STM32 frame timeout must be positive");
    buffer_.reserve(14 + max_payload);
}

void FrameParser::reset() noexcept {
    buffer_.clear();
    candidate_ = false;
}

std::vector<Frame> FrameParser::feed(const std::uint8_t* data, std::size_t size,
                                     Clock::time_point now) {
    std::vector<Frame> frames;
    parse(frames, now);
    for (std::size_t i = 0; i < size; ++i) {
        buffer_.push_back(data[i]);
        parse(frames, now);
    }
    return frames;
}

void FrameParser::parse(std::vector<Frame>& frames, Clock::time_point now) {
    while (!buffer_.empty()) {
        if (buffer_[0] != 0xaa || (buffer_.size() >= 2 && buffer_[1] != 0x55)) {
            buffer_.erase(buffer_.begin());
            candidate_ = false;
            continue;
        }
        if (!candidate_) {
            candidate_ = true;
            candidate_since_ = now;
        }
        if (now - candidate_since_ >= timeout_ || (buffer_.size() >= 3 && buffer_[2] != 1)) {
            buffer_.erase(buffer_.begin());
            candidate_ = false;
            continue;
        }
        if (buffer_.size() < 10)
            return;
        const auto length = read_le(buffer_.data() + 8, 2);
        if (length > max_payload) {
            buffer_.erase(buffer_.begin());
            candidate_ = false;
            continue;
        }
        const auto total = 14 + length;
        if (buffer_.size() < total)
            return;
        if (crc32(buffer_.data() + 2, 8 + length) != read_le(buffer_.data() + 10 + length, 4)) {
            buffer_.erase(buffer_.begin());
            candidate_ = false;
            continue;
        }
        frames.push_back({buffer_[3],
                          read_le(buffer_.data() + 4, 4),
                          {buffer_.begin() + 10, buffer_.begin() + 10 + length}});
        buffer_.erase(buffer_.begin(), buffer_.begin() + total);
        candidate_ = false;
    }
}

} // namespace rkmon::stm32
