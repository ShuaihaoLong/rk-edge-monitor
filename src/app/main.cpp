#include "app/config.hpp"

#include <pthread.h>
#include <signal.h>
#include <atomic>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {
class SignalWaiter {
public:
    explicit SignalWaiter(rkmon::app::Application& application) : application_(application) {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        const int error = pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
        if (error)
            throw std::system_error(error, std::generic_category(), "pthread_sigmask");
        try {
            worker_ = std::thread([this] {
                wait();
            });
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            throw;
        }
    }

    ~SignalWaiter() {
        stopped_ = true;
        worker_.join();
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;

    bool failed() const noexcept {
        return failed_;
    }

private:
    void wait() noexcept {
        while (!stopped_) {
            const timespec timeout{0, 100000000};
            const int signal = sigtimedwait(&signals_, nullptr, &timeout);
            if (signal == SIGINT || signal == SIGTERM) {
                application_.request_stop();
                // 继续消费关闭期间到达的信号，直到应用的工作线程全部退出。
            } else if (signal < 0 && errno != EAGAIN && errno != EINTR) {
                failed_ = true;
                application_.request_stop();
                return;
            }
        }
    }

    rkmon::app::Application& application_;
    sigset_t signals_{}, previous_{};
    std::atomic<bool> stopped_{false}, failed_{false};
    std::thread worker_;
};
}

int main(int argc, char** argv) {
    try {
        std::string path = "config/rkmon.ini";
        bool configured = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help") {
                std::cout << "Usage: rkmon [--config PATH]\n";
                return 0;
            }
            if (arg != "--config" || configured || i + 1 >= argc)
                throw std::invalid_argument("expected --config PATH; see --help");
            path = argv[++i];
            configured = true;
        }
        auto config = rkmon::app::load_config(path);
        rkmon::app::Application application(config.application,
                                            rkmon::app::make_service_factory(config));
        // 信号屏蔽须发生在 Application 创建任何模块线程之前。
        SignalWaiter signals(application);
        const int result = application.run();
        return signals.failed() ? 1 : result;
    } catch (const std::exception& error) {
        std::cerr << "rkmon: " << error.what() << '\n';
        return 1;
    }
}
