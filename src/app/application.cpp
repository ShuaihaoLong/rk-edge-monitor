#include "app/application.hpp"
#include "app/config.hpp"
#include "camera/video_capture_service.hpp"
#include "video/services/video_process_service.hpp"
#include "video/services/video_stream_service.hpp"

#include <exception>
#include <iostream>
#include <type_traits>
#include <utility>

namespace rkmon::app {

Application::ServiceFactory make_service_factory(RuntimeConfig config) {
    return [config = std::move(config)](Application::FaultReporter report,
                                       std::shared_ptr<spdlog::logger> logger) {
        Application::ServiceList services;
        if (config.camera) {
            auto capture = std::make_unique<camera::VideoCaptureService>(
                config.camera->capture, config.camera->queue_capacity,
                [report](const std::string& reason) { report("video_capture", reason); }, logger);
            auto* capture_view = capture.get();
            services.push_back(std::move(capture));
            if (config.video) {
                // 上游先启动；provider 在解码 start 时才读取当次采集队列。
                // ServiceManager 逆序停止/等待，借用的采集服务覆盖解码服务的运行期。
                auto decode = std::make_unique<video::VideoProcessService>(
                    video::make_hardware_decoder(*config.video),
                    [capture_view] { return capture_view->output(); }, *config.video,
                    [report](const std::string& reason) { report("video_decode", reason); }, logger);
                auto* decode_view = decode.get();
                services.push_back(std::move(decode));
                if (config.stream) {
                    services.push_back(std::make_unique<video::VideoStreamService>(
                        video::make_hardware_publisher(*config.stream),
                        [decode_view] { return decode_view->output(); },
                        [report](const std::string& reason) { report("video_stream", reason); }, logger));
                }
            }
        }
        return services;
    };
}

Application::Application() : Application(Options{}) {}

Application::Application(Options options, ServiceFactory factory)
    : options_(std::move(options)),
      factory_(std::move(factory)),
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

        if (control_mailbox_.closed()) {
            shutdown();
            return service_failed_ ? 1 : 0;
        }
        setup_services();
        if (!services_->start_all()) {
            logger_->error("[app] 服务启动失败：{}", services_->last_error());
            result = 1;
        } else {
            logger_->info("[app] services started");
            logger_->flush();
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
    return service_failed_ ? 1 : result;
}

bool Application::request_stop() {
    if (control_mailbox_.closed()) return false;
    control_mailbox_.close(core::CloseMode::discard);
    return true;
}

void Application::setup_services() {
    if (!factory_) return;
    auto services = factory_([this](std::string service, std::string reason) {
        // 致命故障的退出不依赖日志成功或邮箱剩余容量。
        service_failed_ = true;
        try {
            if (control_mailbox_.try_post(ServiceFault{service, reason}) == core::PushResult::accepted)
                return;
            logger_->error("[{}] {}", service, reason);
        } catch (...) {}
        request_stop();
    }, logger_);
    for (auto& service : services) services_->add(std::move(service));
}

void Application::process_control_events() {
    while (auto event = control_mailbox_.receive()) {
        std::visit(
            [this](const auto& value) {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, ShutdownRequested>) {
                    request_stop();
                } else if constexpr (std::is_same_v<Event, ServiceFault>) {
                    service_failed_ = true;
                    logger_->error("[{}] {}", value.service, value.reason);
                    request_stop();
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
