#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rkmon::stm32 {

// 文件描述符仅由串口服务线程访问。
class SerialPort {
public:
    ~SerialPort();
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    void open(const std::string& device, unsigned baud_rate);
    void close() noexcept;

    bool connected() const noexcept {
        return fd_ >= 0;
    }

    std::size_t read(std::uint8_t* data, std::size_t capacity, int timeout_ms);
    void write(const std::uint8_t* data, std::size_t size, int timeout_ms);

private:
    int fd_{-1};
};

} // namespace rkmon::stm32
