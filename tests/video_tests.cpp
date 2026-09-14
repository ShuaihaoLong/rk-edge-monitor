#include "media/services/video_process_service.hpp"
#include <iostream>
#include <stdexcept>

using namespace rkmon;
namespace {
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
class Decoder final : public video::IVideoDecoder {
public:
    std::atomic<bool> stopped{false}, entered{false};
    std::atomic<bool> broken{false}, blocked{false};
    std::atomic<unsigned> opens{0}, closes{0};
    void open() override { ++opens; stopped = false; entered = false; }
    std::optional<camera::VideoFrame> decode(const camera::VideoFrame& frame) override {
        entered = true;
        if (broken) throw std::runtime_error("decode failure");
        while (blocked && !stopped) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stopped) return std::nullopt;
        auto result = frame;
        result.format = camera::PixelFormat::NV12;
        return result;
    }
    void request_stop() noexcept override { stopped = true; }
    void close() noexcept override { ++closes; }
};
template<class F> void wait(F ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready()) {
        check(std::chrono::steady_clock::now() < end, "test wait timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
camera::VideoFrame frame(unsigned seq, std::uint64_t generation = 0) {
    camera::VideoFrame f;
    f.source_generation = generation;
    f.sequence = seq;
    f.data = std::shared_ptr<const std::uint8_t[]>(new std::uint8_t[1]{42});
    f.size = 1;
    return f;
}
}
int main() {
    try {
        using Service = video::VideoProcessService;
        auto input = std::make_shared<Service::Queue>(16);
        auto fake = std::make_unique<Decoder>();
        auto* decoder_view = fake.get();
        std::atomic<unsigned> copies{0};
        Service service(std::move(fake), [&] { return input; }, {100, 2}, {}, {},
                        [&](const camera::VideoFrame& frame) { check(frame.data[0] == 42, "fanout data invalid"); ++copies; });
        check(service.start(), "start failed");
        auto old_output = service.output();
        for (unsigned i = 0; i < 10; ++i) input->push(frame(i));
        wait([&] { return service.stats().frames == 10; });
        wait([&] { return service.stats().dropped == 8; });
        auto held = old_output->pop();
        check(held && held->sequence == 8, "slow consumer did not retain latest frames");
        auto started = std::chrono::steady_clock::now();
        service.request_stop(); service.join();
        check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500), "idle stop blocked");
        check(copies == 10, "fanout stole or lost encoder frames");
        check(!input->closed(), "consumer closed upstream queue");
        check(old_output->closed(), "output not closed");
        input = std::make_shared<Service::Queue>(2);
        check(service.start() && service.output() != old_output, "restart reused output");
        input->push(frame(99, 1));
        auto next = service.output()->pop_for(std::chrono::seconds(1));
        check(next && next->sequence == 99, "restart did not use new upstream queue");
        const auto opens_before_generation_change = decoder_view->opens.load();
        input->push(frame(100, 2));
        next = service.output()->pop_for(std::chrono::seconds(1));
        check(next && next->sequence == 100, "new camera session lost first frame");
        check(decoder_view->opens > opens_before_generation_change, "camera session did not restart decoder");
        service.request_stop(); service.join();
        check(held->data[0] == 42, "held frame lifetime invalid");

        for (bool blocked : {false, true}) {
            auto queue = std::make_shared<Service::Queue>(2);
            auto decoder = std::make_unique<Decoder>();
            auto* view = decoder.get();
            decoder->blocked = blocked;
            decoder->broken = !blocked;
            std::atomic<unsigned> faults{0};
            Service tested(std::move(decoder), [queue] { return queue; }, {},
                           [&](const std::string&) { ++faults; });
            check(tested.start(), "test service start failed");
            queue->push(frame(0));
            wait([&] { return view->entered.load(); });
            if (blocked) {
                started = std::chrono::steady_clock::now();
                tested.request_stop(); tested.join();
                check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500), "decode stop blocked");
                check(faults == 0, "normal stop reported fault");
            } else {
                wait([&] { return tested.health().state == core::ServiceState::degraded; });
                check(faults == 0 && !tested.output()->closed(), "recoverable decoder fault escaped service");
                view->broken = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2100));
                queue->push(frame(1));
                wait([&] { return tested.stats().frames == 1; });
                tested.request_stop(); tested.join();
            }
        }
        auto closed = std::make_shared<Service::Queue>(1);
        closed->close();
        Service invalid(std::make_unique<Decoder>(), [closed] { return closed; });
        check(!invalid.start() && invalid.output()->closed(), "closed upstream accepted");
        std::cout << "video service tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
