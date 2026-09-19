#include "stm32/protocol.hpp"
#include "stm32/stm32_service.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    try {
        using namespace rkmon::stm32;
        const std::string check_text = "123456789";
        check(crc32(reinterpret_cast<const std::uint8_t*>(check_text.data()), check_text.size()) == 0xcbf43926u,
              "CRC32 ISO-HDLC check vector");
        check(crc32(nullptr, 0) == 0, "empty CRC32");
        // 板端实采帧，为编解码提供独立于实现的向量。
        const std::vector<std::uint8_t> wire{0xaa,0x55,1,0x10,0x6a,1,0,0,4,0,0x1c,5,0x2f,0,0x86,0x9e,0xb1,0x0a};
        check(encode({msg_dht11_data, 362, {28,5,47,0}}) == wire, "STM32 firmware wire mismatch");
        FrameParser parser;
        const auto now = FrameParser::Clock::now();
        for (std::size_t i = 0; i + 1 < wire.size(); ++i)
            check(parser.feed(&wire[i], 1, now).empty(), "partial frame accepted");
        const auto frames = parser.feed(&wire.back(), 1, now);
        check(frames.size() == 1 && frames[0].sequence == 362, "fragmented frame failed");
        const auto sample = decode_dht11(frames[0], 1234);
        check(sample && telemetry_json(*sample).find("\"temperature_c\":28.5") != std::string::npos &&
              telemetry_json(*sample).find("\"humidity_percent\":47.0") != std::string::npos, "DHT11 decode");
        check(!decode_dht11({msg_dht11_data,0,{28,10,47,0}},0), "invalid decimal accepted");
        check(!decode_dht11({msg_dht11_data,0,{28,5,47}},0), "short payload accepted");
        check(!decode_dht11({msg_ack,0,{28,5,47,0}},0), "ACK treated as measurement");
        auto corrupted = wire; corrupted[12] ^= 1;
        std::vector<std::uint8_t> stream{1,2,0xaa,0xaa};
        stream.insert(stream.end(), corrupted.begin(), corrupted.end());
        stream.insert(stream.end(), wire.begin(), wire.end());
        stream.insert(stream.end(), wire.begin(), wire.end());
        check(parser.feed(stream.data(), stream.size(), now).size() == 2, "CRC/noise resynchronization");
        auto bad_length = wire; bad_length[8] = 129;
        bad_length.insert(bad_length.end(), wire.begin(), wire.end());
        check(parser.feed(bad_length.data(), bad_length.size(), now).size() == 1, "length bound recovery");
        auto wrong_version = wire; wrong_version[2] = 2;
        check(parser.feed(wrong_version.data(), wrong_version.size(), now).empty(), "unknown version accepted");
        const auto embedded = encode({msg_ack, 7, {0xaa,0x55,1,0xaa,0x55}});
        check(parser.feed(embedded.data(), embedded.size(), now).size() == 1, "embedded sync bytes");
        auto incomplete = wire; incomplete[8] = 128;
        check(parser.feed(incomplete.data(), incomplete.size(), now).empty(), "incomplete accepted");
        check(parser.feed(nullptr,0,now + std::chrono::milliseconds(201)).empty(), "expired frame accepted");
        check(parser.feed(wire.data(),wire.size(),now + std::chrono::milliseconds(202)).size() == 1, "timeout recovery");
        parser.feed(wire.data(),5,now); parser.reset();
        check(parser.feed(wire.data(),wire.size(),now).size() == 1, "reset recovery");
        const auto maximum = encode({msg_cmd_ctrl,0xffffffffu,std::vector<std::uint8_t>(128,0xaa)});
        check(maximum.size() == 142 && parser.feed(maximum.data(),maximum.size(),now).size() == 1, "max payload");
        bool rejected = false;
        try { encode({msg_cmd_ctrl,0,std::vector<std::uint8_t>(129)}); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "oversized encoding accepted");
        std::cout << "STM32 CRC, firmware vector, streaming parser and DHT11 tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
