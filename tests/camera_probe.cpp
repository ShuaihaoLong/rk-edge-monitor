#include "camera/video_capture_service.hpp"
#include "app/config.hpp"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace rkmon::camera;
int main(int argc, char** argv) {
    try {
        std::string config_path = "config/rkmon.ini";
        std::string device;
        int seconds = 10;
        int delay_ms = 0;
        bool yuyv = false;
        auto integer = [](std::string_view text) {
            int value = 0;
            auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 0 || value > 3600000)
                throw std::invalid_argument("invalid numeric argument");
            return value;
        };
        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);
            if (arg == "--help") {
                std::cout << "Usage: rkmon_camera_probe [--config PATH] [--device PATH] [--seconds N] [--delay-ms N] [--yuyv]\n";
                return 0;
            }
            if (arg == "--yuyv") { yuyv = true; continue; }
            if (++i >= argc) throw std::invalid_argument("missing argument");
            if (arg == "--device") device = argv[i];
            else if (arg == "--config") config_path = argv[i];
            else if (arg == "--seconds") seconds = integer(argv[i]);
            else if (arg == "--delay-ms") delay_ms = integer(argv[i]);
            else throw std::invalid_argument("unknown option");
        }
        if (seconds == 0) throw std::invalid_argument("seconds must be positive");
        auto settings = rkmon::app::load_config(config_path);
        if (!settings.camera) throw std::invalid_argument("camera is disabled in configuration");
        auto config = settings.camera->capture;
        if (!device.empty()) config.device = device;
        if (yuyv) { config.format = PixelFormat::YUYV; config.width = 640; config.height = 480; }
        VideoCaptureService service(config, settings.camera->queue_capacity);
        if (!service.start()) throw std::runtime_error(service.health().detail);
        const auto format = service.negotiated_format();
        std::cout << "negotiated=" << format.width << 'x' << format.height
                  << " format=" << static_cast<int>(format.format) << " stride=" << format.stride
                  << " interval=" << format.interval_numerator << '/' << format.interval_denominator << '\n';
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::uint64_t consumed = 0, bytes = 0;
        auto queue = service.output();
        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = queue->pop();
            if (!frame) break;
            if (!frame->data || frame->size == 0) throw std::runtime_error("empty frame");
            if (frame->format == PixelFormat::MJPG &&
                (frame->size < 2 || frame->data[0] != 0xff || frame->data[1] != 0xd8))
                throw std::runtime_error("missing JPEG SOI marker");
            if (frame->format == PixelFormat::YUYV && frame->size < frame->stride * frame->height)
                throw std::runtime_error("short YUYV frame");
            ++consumed;
            bytes += frame->size;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        service.request_stop(); service.join();
        const auto stats = service.stats();
        std::cout << "captured=" << stats.frames << " consumed=" << consumed << " bytes=" << bytes
                  << " fps=" << stats.fps << " queue_dropped=" << stats.dropped
                  << " timeouts=" << stats.timeouts << '\n';
        if (service.health().state == rkmon::core::ServiceState::failed)
            throw std::runtime_error(service.health().detail);
        if (consumed == 0) throw std::runtime_error("no frames consumed");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
