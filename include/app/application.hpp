#pragma once

#include "app/control_event.hpp"
#include "core/logger.hpp"
#include "core/mailbox.hpp"
#include "core/service_manager.hpp"

#include <cstddef>
#include <memory>

namespace rkmon::app {

class Application {
public:
    struct Options {
        log::Options log;
        std::size_t control_mailbox_capacity{64};
    };

    Application();
    explicit Application(Options options);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 一个实例只运行一次，在调用线程中处理控制消息，直到收到停止请求。
    int run();

    // 可由其他普通线程调用；关闭邮箱唤醒 run，不依赖队列剩余容量。
    // 返回 false 表示邮箱已经关闭。不能在 POSIX 原始信号处理函数中调用。
    bool request_stop();

private:
    void setup_services();
    void process_control_events();
    void shutdown() noexcept;

    Options options_;

    // 邮箱的生命周期必须覆盖所有借用它的服务。
    core::Mailbox<ControlEvent> control_mailbox_;
    std::unique_ptr<core::ServiceManager> services_;
    std::shared_ptr<spdlog::logger> logger_;

    bool run_called_{false};
    bool logger_initialized_{false};
};

} // namespace rkmon::app
