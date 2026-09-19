#include "app/config.hpp"
#include "camera/video_capture_service.hpp"
#include "media/services/video_process_service.hpp"
#include <iostream>
#include "media/nv12.hpp"
#include <stdexcept>
#include <thread>
#include <sys/resource.h>

// 板端有限时长验证：只校验内存，不写相机图像。
int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 4) {
            std::cerr << "Usage: rkmon_video_probe CONFIG [SECONDS=10] [DELAY_MS=0]\n";
            return 2;
        }
        const int seconds = argc > 2 ? std::stoi(argv[2]) : 10;
        const int delay = argc > 3 ? std::stoi(argv[3]) : 0;
        if (seconds <= 0 || delay < 0) throw std::invalid_argument("invalid duration/delay");
        const auto config = rkmon::app::load_config(argv[1]);
        if (!config.camera || !config.video) throw std::runtime_error("camera and video must be enabled");
        rkmon::camera::VideoCaptureService capture(config.camera->capture, config.camera->queue_capacity);
        rkmon::video::VideoProcessService decode(rkmon::video::make_hardware_decoder(*config.video),
                                                [&] { return capture.output(); }, *config.video);
        if (!capture.start()) throw std::runtime_error(capture.health().detail);
        if (!decode.start()) throw std::runtime_error(decode.health().detail);
        rusage usage_begin{};
        getrusage(RUSAGE_SELF, &usage_begin);
        double latency_ms = 0;
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + std::chrono::seconds(seconds);
        auto queue = decode.output();
        std::optional<rkmon::camera::VideoFrame> held;
        std::uint8_t held_byte = 0;
        std::uint64_t consumed = 0, previous = 0;
        unsigned long long checksum = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = queue->pop_for(std::chrono::milliseconds(100));
            if (!frame) {
                if (queue->closed()) break;
                continue;
            }
            rkmon::media::validate_nv12(*frame);
            std::unique_ptr<rkmon::media::DmaMapping> mapping;
            if(frame->dma)mapping=std::make_unique<rkmon::media::DmaMapping>(frame->dma);
            const auto* pixels=mapping?mapping->data():frame->data.get();
            if (consumed && frame->sequence <= previous) throw std::runtime_error("frame sequence regressed");
            previous = frame->sequence;
            latency_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame->timestamp).count();
            for (std::size_t i = 0; i < frame->size; i += 4096) checksum += pixels[i];
            if (!held) { held = *frame; held_byte = pixels[0]; }
            ++consumed;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        decode.request_stop(); capture.request_stop();
        decode.join(); capture.join();
        if(held) {
            std::unique_ptr<rkmon::media::DmaMapping> mapping;
            if(held->dma)mapping=std::make_unique<rkmon::media::DmaMapping>(held->dma);
            if((mapping?mapping->data():held->data.get())[0]!=held_byte)throw std::runtime_error("invalid retained frame");
        }
        const auto stats = decode.stats();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        rusage usage_end{};
        getrusage(RUSAGE_SELF, &usage_end);
        auto cpu_seconds = [](const rusage& u) {
            return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
        };
        std::cout << "captured=" << capture.stats().frames << " decoded=" << stats.frames
                  << " consumed=" << consumed << " dropped=" << stats.dropped
                  << " decode_fps=" << stats.frames / elapsed << " checksum=" << checksum
                  << " mean_latency_ms=" << (consumed ? latency_ms / consumed : 0)
                  << " cpu_percent=" << 100 * (cpu_seconds(usage_end) - cpu_seconds(usage_begin)) / elapsed
                  << " max_rss_kib=" << usage_end.ru_maxrss << '\n';
        if (decode.health().state == rkmon::core::ServiceState::failed) throw std::runtime_error(decode.health().detail);
        if (capture.health().state == rkmon::core::ServiceState::failed) throw std::runtime_error(capture.health().detail);
        if (!consumed) throw std::runtime_error("no decoded frames");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
