#include "app/application.hpp"

#include <exception>
#include <iostream>
#include <type_traits>
#include <utility>

namespace rkmon::app {

Application::Application() : Application(Options{}) {}

Application::Application(Options options)
    : options_(std::move(options)),
      control_mailbox_(options_.control_mailbox_capacity),
      services_(std::make_unique<core::ServiceManager>()) {}

Application::~Application() { shutdown(); }

int Application::run() {
    if (run_called_) {
        return 2;
    }
    run_called_ = true;
    int result = 0;

    try {
        log::init(options_.log);
        logger_initialized_ = true;
        logger_ = log::get();

        setup_services();
        if (!services_->start_all()) {
            logger_->error("[app] 服务启动失败：{}", services_->last_error());
            result = 1;
        } else {
            process_control_events();
        }
    } catch (const std::exception& error) {
        // 日志初始化本身也可能失败，异常出口不依赖 logger。
        std::cerr << "rkmon: " << error.what() << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "rkmon: 未知异常\n";
        result = 1;
    }
    shutdown();
    return result;
}

bool Application::request_stop() {
    if (control_mailbox_.closed()) return false;
    control_mailbox_.close(core::CloseMode::discard);
    return true;
}

void Application::setup_services() {
}

void Application::process_control_events() {
    while (auto event = control_mailbox_.receive()) {
        std::visit(
            [this](const auto& value) {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, ShutdownRequested>) {
                    request_stop();
                } else if constexpr (std::is_same_v<Event, ServiceFault>) {
                    logger_->error("[{}] {}", value.service, value.reason);
                }
            },
            *event);
    }
}

void Application::shutdown() noexcept {
    // 服务必须先停止并销毁，保证工作线程不再使用 mailbox 和 logger。
    if (services_) {
        services_->stop_all();
        services_.reset();
    }
    control_mailbox_.close(core::CloseMode::discard);
    logger_.reset();
    if (logger_initialized_) {
        log::shutdown();
        logger_initialized_ = false;
    }
}

} // namespace rkmon::app
