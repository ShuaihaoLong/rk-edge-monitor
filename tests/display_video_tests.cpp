#include "display/ad_reader.hpp"
#include "display/rga_converter.hpp"
#include "media/dma_buffer.hpp"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <unistd.h>
#include <cstring>
#include "display/video_reader.hpp"
#include <gst/gst.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check_colors() {
    constexpr int stride = 1280, height = 720, padded_height = 736;
    constexpr int bytes = stride * padded_height * 3 / 2;
    auto dma = rkmon::media::allocate_dma_buffer(bytes);
    rkmon::display::RgaConverter converter;
    for (bool bt709 : {false, true}) {
        {
            rkmon::media::DmaMapping mapped(dma, true);
            std::memset(mapped.data(), 0, bytes);
            for (int y = 0; y < height; ++y)
                std::memset(mapped.data() + y * stride, bt709 ? 63 : 81, stride);
            for (int y = 0; y < height / 2; ++y)
                for (int x = 0; x < stride; x += 2) {
                    mapped.data()[stride * padded_height + y * stride + x] = bt709 ? 102 : 90;
                    mapped.data()[stride * padded_height + y * stride + x + 1] = 240;
                }
            mapped.finish();
        }
        const int fd = dup(dma->fd);
        if (fd < 0) throw std::runtime_error("dup failed");
        auto* allocator = gst_dmabuf_allocator_new();
        auto* memory = gst_dmabuf_allocator_alloc(allocator, fd, bytes);
        gst_object_unref(allocator);
        if (!memory) {
            close(fd);
            throw std::runtime_error("GStreamer DMA import failed");
        }
        auto* buffer = gst_buffer_new();
        gst_buffer_append_memory(buffer, memory);
        gsize offsets[GST_VIDEO_MAX_PLANES]{0, stride * padded_height};
        gint strides[GST_VIDEO_MAX_PLANES]{stride, stride};
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
                                       stride, height, 2, offsets, strides);
        auto* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
                                        "width", G_TYPE_INT, stride, "height", G_TYPE_INT, height,
                                        "colorimetry", G_TYPE_STRING, bt709 ? "bt709" : "bt601", nullptr);
        std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample(
            gst_sample_new(buffer, caps, nullptr, nullptr), gst_sample_unref);
        gst_buffer_unref(buffer);
        gst_caps_unref(caps);
        const auto result = converter.convert(sample.get());
        for (int y : {10, 216, 420}) {
            const auto* pixel = result.pixels.data() + (y * 768 + 384) * 4;
            if (pixel[0] > 15 || pixel[1] > 15 || pixel[2] < 240 || pixel[3] != 255)
                throw std::runtime_error("RGA red color/stride/BGRA validation failed");
        }
    }
    std::cout << "PASS RGA BT601/BT709 color and padded stride\n";
}

double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage))
        throw std::runtime_error("getrusage failed");
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
struct Frames {
    unsigned count{};
    std::chrono::steady_clock::time_point last{};
    std::vector<double> gaps;
    void record(const rkmon::display::VideoImage& frame) {
        if (frame.pixels.size() != 768 * 432 * 4)
            throw std::runtime_error("incorrect BGRA frame size");
        if (count)
            gaps.push_back(std::chrono::duration<double, std::milli>(frame.received - last).count());
        last = frame.received;
        ++count;
    }
    bool report(const char* name, double elapsed) {
        std::sort(gaps.begin(), gaps.end());
        const double p95 = gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100];
        std::cout << name << " frames=" << count << " fps=" << count / elapsed
                  << " gap_p95_ms=" << p95 << '\n';
        return count / elapsed >= 27 && p95 < 100;
    }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("Usage: display_video_tests RTSP_URL ADS_ROOT SECONDS [both|live|ad]");
        gst_init(nullptr, nullptr);
        check_colors();
        const int seconds = std::stoi(argv[3]);
        const std::string mode = argc == 5 ? argv[4] : "both";
        if (seconds < 5 || seconds > 300 || (mode != "both" && mode != "live" && mode != "ad"))
            throw std::runtime_error("seconds must be in [5,300], mode must be both/live/ad");
        std::unique_ptr<rkmon::display::VideoReader> live;
        std::unique_ptr<rkmon::display::AdReader> ads;
        if (mode != "ad") {
            live = std::make_unique<rkmon::display::VideoReader>(argv[1]);
            live->set_enabled(true);
        }
        if (mode != "live")
            ads = std::make_unique<rkmon::display::AdReader>(argv[2]);
        // 测量窗口排除 RTSP 建连和首个关键帧等待；验收输入应为 30 fps。
        std::this_thread::sleep_for(std::chrono::seconds(3));
        rkmon::display::VideoImage frame;
        if (live) live->take(frame);
        if (ads) ads->take(frame);
        Frames live_frames, ad_frames;
        const auto start = std::chrono::steady_clock::now();
        const double cpu_start = cpu_seconds();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            if (live && live->take(frame)) live_frames.record(frame);
            if (ads && ads->take(frame)) ad_frames.record(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "cpu_percent=" << (cpu_seconds() - cpu_start) / elapsed * 100 << '\n';
        bool passed = true;
        if (live) passed &= live_frames.report("live", elapsed);
        if (ads) passed &= ad_frames.report("ad", elapsed);
        if (!passed)
            throw std::runtime_error("30 fps display reader performance check failed");
        std::cout << "PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
