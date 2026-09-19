#pragma once

#include <string>

namespace rkmon::stm32 {
struct Config {
    std::string device{"/dev/ttyS9"};
    unsigned baud_rate{115200};
    unsigned frame_timeout_ms{200};
    unsigned stale_timeout_ms{5000};
    unsigned reconnect_interval_ms{2000};
};
} // namespace rkmon::stm32
