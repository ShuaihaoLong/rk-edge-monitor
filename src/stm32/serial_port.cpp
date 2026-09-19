#include "serial_port.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace rkmon::stm32 {
namespace {
[[noreturn]] void fail(const char* action) {
    throw std::runtime_error(std::string("STM32 serial ") + action + ": " + std::strerror(errno));
}
speed_t baud(unsigned value) {
    switch (value) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: throw std::invalid_argument("unsupported STM32 baud rate");
    }
}
}

SerialPort::~SerialPort() { close(); }

void SerialPort::open(const std::string& device, unsigned baud_rate) {
    close();
    const auto speed = baud(baud_rate);
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) fail("open");
    try {
        if (ioctl(fd_, TIOCEXCL) < 0) fail("exclusive access");
        termios options{};
        if (tcgetattr(fd_, &options) < 0) fail("get attributes");
        cfmakeraw(&options);
        options.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
        options.c_cflag |= CS8 | CLOCAL | CREAD;
        options.c_cc[VMIN] = 0;
        options.c_cc[VTIME] = 0;
        if (cfsetispeed(&options, speed) < 0 || cfsetospeed(&options, speed) < 0 ||
            tcsetattr(fd_, TCSANOW, &options) < 0) fail("configure");
        if (tcflush(fd_, TCIOFLUSH) < 0) fail("flush");
    } catch (...) { close(); throw; }
}

void SerialPort::close() noexcept {
    if (fd_ >= 0) {
        ioctl(fd_, TIOCNXCL);
        ::close(fd_);
    }
    fd_ = -1;
}

std::size_t SerialPort::read(std::uint8_t* data, std::size_t capacity, int timeout_ms) {
    if (fd_ < 0) throw std::logic_error("STM32 serial disconnected");
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0) { if (errno == EINTR) return 0; fail("poll"); }
    if (!ready) return 0;
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw std::runtime_error("STM32 serial disconnected");
    const auto count = ::read(fd_, data, capacity);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        fail("read");
    }
    return static_cast<std::size_t>(count);
}

void SerialPort::write(const std::uint8_t* data, std::size_t size, int timeout_ms) {
    if (fd_ < 0) throw std::logic_error("STM32 serial disconnected");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("STM32 serial write timeout; command outcome unknown");
        pollfd descriptor{fd_, POLLOUT, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0) { if (errno == EINTR) continue; fail("write poll"); }
        if (!ready) continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("STM32 serial disconnected during write; command outcome unknown");
        const auto sent = ::write(fd_, data, size);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fail("write");
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

} // namespace rkmon::stm32
