#include "rga_converter.hpp"
#include "video_reader.hpp"
#include <gst/app/gstappsink.h>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace rkmon::display {
namespace {
struct Pipeline {
    GstElement* pipeline{};
    GstAppSink* sink{};

    ~Pipeline() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
        }
        if (sink)
            gst_object_unref(sink);
        if (pipeline)
            gst_object_unref(pipeline);
    }
};
}

VideoReader::VideoReader(std::string url)
    : url_(std::move(url)), thread_(&VideoReader::run, this) {}

VideoReader::~VideoReader() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void VideoReader::set_enabled(bool enabled) {
    enabled_ = enabled;
}

bool VideoReader::take(VideoImage& frame) {
    std::lock_guard lock(mutex_);
    if (latest_.pixels.empty())
        return false;
    frame = std::move(latest_);
    return true;
}

std::string VideoReader::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void VideoReader::run() noexcept {
    std::uint64_t sequence = 0;
    while (!stop_) {
        if (!enabled_) {
            {
                std::lock_guard lock(mutex_);
                latest_.pixels.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        try {
            RgaConverter converter;
            Pipeline p;
            GError* error = nullptr;
            p.pipeline = gst_parse_launch(
                "rtspsrc name=source protocols=tcp latency=0 tcp-timeout=3000000 "
                "! rtph264depay ! h264parse ! mppvideodec dma-feature=true arm-afbc=false "
                "! video/x-raw(ANY),format=NV12 "
                "! appsink name=frames sync=false max-buffers=1 drop=true wait-on-eos=false",
                &error);
            if (error) {
                std::string why = error->message;
                g_error_free(error);
                throw std::runtime_error(why);
            }
            if (!p.pipeline)
                throw std::runtime_error("pipeline creation failed");
            auto* source = gst_bin_get_by_name(GST_BIN(p.pipeline), "source");
            g_object_set(source, "location", url_.c_str(), nullptr);
            gst_object_unref(source);
            p.sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(p.pipeline), "frames"));
            if (gst_element_set_state(p.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("video startup failed");
            auto last = std::chrono::steady_clock::now();
            bool announced = false;
            while (!stop_ && enabled_) {
                auto* sample = gst_app_sink_try_pull_sample(p.sink, 20 * GST_MSECOND);
                if (sample) {
                    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owned(
                        sample, gst_sample_unref);
                    auto next = converter.convert(sample);
                    next.sequence = ++sequence;
                    if (!announced) {
                        std::cout << "[display/video] connected 768x432 BGRA (MPP + RGA3)" << std::endl;
                        announced = true;
                    }
                    last = std::chrono::steady_clock::now();
                    std::lock_guard lock(mutex_);
                    latest_ = std::move(next);
                    status_ = "实时画面";
                }
                auto* bus = gst_element_get_bus(p.pipeline);
                auto* msg = gst_bus_pop_filtered(
                    bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                gst_object_unref(bus);
                if (msg) {
                    std::string why = "video EOS";
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        GError* ge = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_error(msg, &ge, &debug);
                        why = ge->message;
                        g_error_free(ge);
                        g_free(debug);
                    }
                    gst_message_unref(msg);
                    throw std::runtime_error(why);
                }
                if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
                    throw std::runtime_error("video frame timeout");
            }
        } catch (const std::exception& error) {
            std::cerr << "[display/video] " << error.what() << '\n';
            std::lock_guard lock(mutex_);
            status_ = "视频离线 · 正在重连";
            latest_.pixels.clear();
        }
        for (int i = 0; i < 100 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
}
