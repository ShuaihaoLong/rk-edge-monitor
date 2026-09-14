#include "media/services/video_stream_service.hpp"
#include <iostream>
#include <stdexcept>

using namespace rkmon;
namespace {
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
template<class F> void wait(F ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, "wait timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class Publisher final : public video::IVideoPublisher {
public:
    std::atomic<unsigned> writes{0}, checks{0};
    std::atomic<bool> broken{false}, stopped{false};
    bool fail_open{false};
    void open() override {
        if (fail_open) throw std::runtime_error("open failed");
        stopped = false;
    }
    void write(const camera::VideoFrame&) override { ++writes; }
    void check_health() override {
        ++checks;
        if (broken) throw std::runtime_error("connection lost");
    }
    void request_stop() noexcept override { stopped = true; }
    void close() noexcept override {}
    std::uint64_t encoded_frames() const noexcept override { return writes; }
};
}
int main() {
    try {
        using Service = video::VideoStreamService;
        auto queue = std::make_shared<Service::Queue>(2);
        auto publisher = std::make_unique<Publisher>();
        auto* view = publisher.get();
        std::atomic<unsigned> faults{0};
        Service service(std::move(publisher), [&] { return queue; }, [&](const std::string&) { ++faults; });
        check(service.start(), "start failed");
        queue->push(camera::VideoFrame{});
        wait([&] { return view->writes == 1; });
        auto start = std::chrono::steady_clock::now();
        service.request_stop(); service.join();
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500), "idle stop blocked");
        check(!queue->closed() && faults == 0, "stop closed upstream or reported fault");
        auto old_queue = queue;
        queue = std::make_shared<Service::Queue>(2);
        check(service.start(), "restart failed");
        old_queue->close();
        queue->push(camera::VideoFrame{});
        wait([&] { return view->writes == 2; });
        // 网络故障降级并在后台重连，不能终止进程或关闭输入队列。
        view->broken = true;
        wait([&] { return service.health().state == core::ServiceState::degraded; });
        check(service.health().detail == "connection lost", "fault reason lost");
        view->broken = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        queue->push(camera::VideoFrame{});
        wait([&] { return view->writes == 3; });
        check(faults == 0 && !queue->closed(), "recoverable stream fault propagated");
        service.request_stop(); service.join();
        queue->close();
        check(!service.start(), "closed upstream accepted");
        queue = std::make_shared<Service::Queue>(2);
        view->fail_open = true;
        check(service.start(), "publisher outage prevented service startup");
        wait([&] { return service.health().state == core::ServiceState::degraded; });
        service.request_stop(); service.join();
        std::cout << "stream service tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
