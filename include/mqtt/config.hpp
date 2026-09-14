#pragma once

#include <string>

namespace rkmon::mqtt {

struct Config {
    std::string broker_host{"127.0.0.1"};
    unsigned broker_port{1883};
    std::string device_id;
    std::string role{"edge"};
    std::string topic_prefix{"rkmon/devices"};
    unsigned publish_interval_ms{2000};
    unsigned keepalive_seconds{10};
};

} // namespace rkmon::mqtt
