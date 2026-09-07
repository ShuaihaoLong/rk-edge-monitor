#pragma once

#include <string>
#include <variant>

namespace rkmon::app {

// 工作线程只上报事实，由 Application 在主控制线程中决定如何处理。
struct ShutdownRequested {};

struct ServiceFault {
    std::string service;
    std::string reason;
};

using ControlEvent = std::variant<ShutdownRequested, ServiceFault>;

} // namespace rkmon::app
