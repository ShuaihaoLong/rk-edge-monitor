#include "camera/video_capture_service.hpp"
#include "camera/v4l2_video_source.hpp"

#include <condition_variable>
#include <iostream>
#include <stdexcept>

using namespace rkmon::camera;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("condition timed out");
        std::this_thread::sleep_for(1ms);
    }
}
class FakeSource final : public IVideoSource {
public:
    enum class Mode { frames, blocked, timeout, error, open_failure };
    explicit FakeSource(Mode mode) : mode_(mode) {}
    void open() override {
        if (mode_ == Mode::open_failure) throw std::runtime_error("open failed");
        std::lock_guard lock(mutex_);
        stopped_ = false;
        sequence_ = 0;
    }
    ReadResult read() override {
        std::unique_lock lock(mutex_);
        if (mode_ == Mode::blocked) cv_.wait(lock, [&] { return stopped_; });
        else cv_.wait_for(lock, 1ms, [&] { return stopped_; });
        if (stopped_) return {ReadStatus::stopped, {}, {}};
        if (mode_ == Mode::timeout) return {ReadStatus::timeout, {}, {}};
        if (mode_ == Mode::error) return {ReadStatus::error, {}, "device disconnected"};
        VideoFrame frame;
        frame.sequence = sequence_++;
        frame.data = std::shared_ptr<const std::uint8_t[]>(new std::uint8_t[1]{42});
        frame.size = 1;
        return {ReadStatus::frame, std::move(frame), {}};
    }
    void request_stop() noexcept override {
        { std::lock_guard lock(mutex_); stopped_ = true; }
        cv_.notify_all();
    }
    void close() noexcept override { ++closed; }
    NegotiatedFormat negotiated_format() const override { return {}; }
    unsigned closed{0};
private:
    Mode mode_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_{false};
    std::uint64_t sequence_{0};
};
void queue_and_restart() {
    VideoCaptureService service(std::make_unique<FakeSource>(FakeSource::Mode::frames), 2);
    check(service.start(), "start failed");
    auto old_queue = service.output();
    eventually([&] { return old_queue->dropped() >= 8; });
    auto held_frame = old_queue->pop();
    check(held_frame && held_frame->sequence >= 8, "queue did not retain recent frames");
    check(service.stats().queue_depth <= 2, "queue capacity exceeded");
    service.request_stop(); service.join();
    check(old_queue->closed() && !old_queue->pop(), "stop did not discard and close queue");
    check(service.start(), "restart failed");
    check(service.output() != old_queue, "restart reused a closed queue");
    eventually([&] { return service.stats().frames >= 2; });
    service.request_stop(); service.request_stop(); service.join(); service.join();
    check(held_frame->data[0] == 42, "frame lifetime depends on source or queue");
}
void blocked_stop() {
    VideoCaptureService service(std::make_unique<FakeSource>(FakeSource::Mode::blocked));
    check(service.start(), "blocked source start failed");
    const auto started = std::chrono::steady_clock::now();
    service.request_stop(); service.join();
    check(std::chrono::steady_clock::now() - started < 500ms, "stop failed to wake read");
}
void failures() {
    for (auto mode : {FakeSource::Mode::timeout, FakeSource::Mode::error, FakeSource::Mode::open_failure}) {
        std::atomic<unsigned> faults{0};
        auto source = std::make_unique<FakeSource>(mode);
        auto* raw = source.get();
        VideoCaptureService service(std::move(source), 2, 3,
                                    [&](const std::string&) { ++faults; });
        const bool started = service.start();
        check(started == (mode != FakeSource::Mode::open_failure), "wrong start result");
        eventually([&] { return faults == 1; });
        service.join();
        check(raw->closed > 0, "failure did not close source");
        check(service.health().state == rkmon::core::ServiceState::failed, "failure state lost");
        check(!service.health().detail.empty(), "missing diagnostic");
        check(service.output()->closed(), "failure left consumer blocked");
        if (mode == FakeSource::Mode::timeout) check(service.stats().timeouts == 3, "timeout threshold wrong");
    }
    CaptureConfig config;
    config.device = "/dev/rkmon-nonexistent-camera";
    V4L2VideoSource source(config);
    for (int i = 0; i < 2; ++i) {
        bool failed = false;
        try { source.open(); } catch (const std::system_error&) { failed = true; }
        check(failed, "missing device unexpectedly opened");
        source.request_stop(); source.close(); source.close();
    }
}
}
int main() {
    try { queue_and_restart(); blocked_stop(); failures(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "camera tests passed\n";
}
