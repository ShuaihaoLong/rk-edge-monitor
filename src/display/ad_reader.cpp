#include "rga_converter.hpp"
#include "ad_reader.hpp"
#include <gst/app/gstappsink.h>
#include <json-c/json.h>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <filesystem>
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

AdReader::AdReader(std::string root)
    : root_(std::move(root)), advertising_(mode() == "ad" && !playlist().empty()),
      thread_(&AdReader::run, this) {}

AdReader::~AdReader() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool AdReader::take(VideoImage& frame) {
    std::lock_guard lock(mutex_);
    if (latest_.pixels.empty())
        return false;
    frame = std::move(latest_);
    return true;
}

bool AdReader::advertising() const {
    return advertising_.load();
}

std::string AdReader::mode() const {
    std::ifstream input(std::filesystem::path(root_) / "display-mode");
    std::string value;
    std::getline(input, value);
    return value == "live" ? "live" : "ad";
}

std::vector<std::string> AdReader::playlist() const {
    std::unique_ptr<json_object, decltype(&json_object_put)> object(
        json_object_from_file((std::filesystem::path(root_) / "playlist.json").c_str()),
        json_object_put);
    if (!object || !json_object_is_type(object.get(), json_type_array))
        return {};
    std::vector<std::string> result;
    for (size_t i = 0; i < json_object_array_length(object.get()); ++i) {
        auto* item = json_object_array_get_idx(object.get(), i);
        if (!json_object_is_type(item, json_type_string))
            continue;
        const auto name = std::string(json_object_get_string(item));
        const auto path = (std::filesystem::path(root_) / "videos" / name).lexically_normal();
        if (path.parent_path() == std::filesystem::path(root_) / "videos" &&
            path.extension() == ".mp4" && std::filesystem::is_regular_file(path))
            result.push_back(path.string());
    }
    return result;
}

void AdReader::run() noexcept {
    std::uint64_t sequence = 0;
    size_t index = 0;
    std::string current_path;
    while (!stop_) {
        const auto paths = playlist();
        if (mode() != "ad" || paths.empty()) {
            advertising_ = false;
            {
                std::lock_guard lock(mutex_);
                latest_.pixels.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        advertising_ = true;
        if (index >= paths.size() ||
            (!current_path.empty() &&
             std::find(paths.begin(), paths.end(), current_path) == paths.end()))
            index = 0;
        current_path = paths[index];
        try {
            RgaConverter converter;
            Pipeline pipeline;
            GError* error = nullptr;
            pipeline.pipeline = gst_parse_launch(
                "filesrc name=file ! qtdemux name=demux demux.video_0 ! h264parse "
                "! mppvideodec dma-feature=true arm-afbc=false "
                "! video/x-raw(ANY),format=NV12 "
                "! appsink name=frames sync=true max-buffers=1 drop=true wait-on-eos=false",
                &error);
            if (error) {
                std::string why = error->message;
                g_error_free(error);
                throw std::runtime_error(why);
            }
            if (!pipeline.pipeline)
                throw std::runtime_error("advertisement pipeline creation failed");
            auto* file = gst_bin_get_by_name(GST_BIN(pipeline.pipeline), "file");
            g_object_set(file, "location", current_path.c_str(), nullptr);
            gst_object_unref(file);
            pipeline.sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline.pipeline), "frames"));
            if (gst_element_set_state(pipeline.pipeline, GST_STATE_PLAYING) ==
                GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("advertisement startup failed");
            bool ended = false;
            bool announced = false;
            auto last_frame = std::chrono::steady_clock::now();
            while (!stop_ && mode() == "ad") {
                auto* sample = gst_app_sink_try_pull_sample(pipeline.sink, 50 * GST_MSECOND);
                if (sample) {
                    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owned(
                        sample, gst_sample_unref);
                    auto next = converter.convert(sample);
                    next.sequence = ++sequence;
                    last_frame = next.received;
                    if (!announced) {
                        std::cout << "[display/ad] playing " << current_path << " 768x432 BGRA (MPP + RGA3)"
                                  << std::endl;
                        announced = true;
                    }
                    std::lock_guard lock(mutex_);
                    latest_ = std::move(next);
                }
                auto* bus = gst_element_get_bus(pipeline.pipeline);
                auto* message = gst_bus_pop_filtered(
                    bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                gst_object_unref(bus);
                if (message) {
                    ended = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
                    std::string reason;
                    if (!ended) {
                        GError* detail = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_error(message, &detail, &debug);
                        reason = detail ? detail->message : "advertisement pipeline error";
                        g_clear_error(&detail);
                        g_free(debug);
                    }
                    gst_message_unref(message);
                    if (!ended)
                        throw std::runtime_error(reason);
                    break;
                }
                if (std::chrono::steady_clock::now() - last_frame > std::chrono::seconds(10))
                    throw std::runtime_error("advertisement frame timeout");
            }
            if (ended)
                index = (index + 1) % paths.size();
        } catch (const std::exception& error) {
            std::cerr << "[display/ad] " << error.what() << '\n';
            index = (index + 1) % paths.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    advertising_ = false;
}
}
