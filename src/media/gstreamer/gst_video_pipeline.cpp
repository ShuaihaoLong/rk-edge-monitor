#include "media/gstreamer/gst_video_pipeline.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rkmon::video {
namespace {
// 样本和映射在异常路径同样释放，输出帧不借用 GstBuffer 的内存。
struct Sample {
    GstSample* value;
    ~Sample() { if (value) gst_sample_unref(value); }
};
struct MappedFrame {
    GstVideoFrame value{};
    bool mapped{false};
    ~MappedFrame() { if (mapped) gst_video_frame_unmap(&value); }
};
}

struct GstVideoPipeline::Impl {
    DecodeConfig config;
    std::atomic<bool> stop{false};
    GstElement* pipeline{nullptr};
    GstElement* source{nullptr};
    GstElement* sink{nullptr};
    GstBus* bus{nullptr};
    int width{0}, height{0};
    std::chrono::steady_clock::time_point origin{};

    void check_bus() {
        auto* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message) {
            return;
        }
        std::string reason = "unexpected decoder EOS";
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            reason = error ? error->message : "GStreamer decoder error";
            if (debug) {
                reason += std::string("; ") + debug;
            }
            g_clear_error(&error);
            g_free(debug);
        }
        gst_message_unref(message);
        throw std::runtime_error(reason);
    }
};

GstVideoPipeline::GstVideoPipeline(DecodeConfig config) : impl_(std::make_unique<Impl>()) {
    if (config.timeout_ms == 0) {
        throw std::invalid_argument("decode timeout must be positive");
    }
    impl_->config = config;
}

GstVideoPipeline::~GstVideoPipeline() { close(); }

void GstVideoPipeline::open() {
    auto& s = *impl_;
    if (s.pipeline) {
        throw std::logic_error("decoder already open");
    }
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        const std::string reason = error ? error->message : "gst_init_check failed";
        g_clear_error(&error);
        throw std::runtime_error(reason);
    }
    s.stop = false;
    try {
        // 每次仅一个请求在途，入口不阻塞；appsink 也限制为一帧，不允许静默丢帧。
        // 厂商插件须显式设置 format，单独的 capsfilter 不会可靠触发 NV16→NV12。
        s.pipeline = gst_parse_launch(
            "appsrc name=input is-live=true format=time block=false max-buffers=1 max-bytes=0 "
            "! jpegparse ! mppjpegdec format=NV12 "
            "! video/x-raw,format=NV12 "
            "! appsink name=output sync=false max-buffers=1 drop=false wait-on-eos=false", &error);
        if (error || !s.pipeline) {
            const std::string reason = error ? error->message : "cannot create decoder pipeline";
            g_clear_error(&error);
            throw std::runtime_error(reason);
        }
        s.source = gst_bin_get_by_name(GST_BIN(s.pipeline), "input");
        s.sink = gst_bin_get_by_name(GST_BIN(s.pipeline), "output");
        s.bus = gst_element_get_bus(s.pipeline);
        if (gst_element_set_state(s.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            s.check_bus();
            throw std::runtime_error("cannot start decoder pipeline");
        }
    } catch (...) {
        close();
        throw;
    }
}

std::optional<camera::VideoFrame> GstVideoPipeline::decode(const camera::VideoFrame& input) {
    auto& s = *impl_;
    if (s.stop) {
        return std::nullopt;
    }
    if (!s.pipeline) {
        throw std::logic_error("decoder is not open");
    }
    if (input.format != camera::PixelFormat::MJPG || !input.data || input.size < 4 ||
        input.width <= 0 || input.height <= 0 || input.width > 16384 || input.height > 16384 ||
        input.width % 2 || input.height % 2) {
        throw std::invalid_argument("decoder requires a nonempty MJPEG frame with even dimensions");
    }
    s.check_bus();
    if (s.width == 0) {
        s.width = input.width;
        s.height = input.height;
        s.origin = input.timestamp;
        auto* caps = gst_caps_new_simple("image/jpeg", "width", G_TYPE_INT, s.width,
                                        "height", G_TYPE_INT, s.height, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(s.source), caps);
        gst_caps_unref(caps);
    }
    if (input.width != s.width || input.height != s.height || input.timestamp < s.origin) {
        throw std::runtime_error("input format or timestamp changed; restart decoder");
    }
    auto* buffer = gst_buffer_new_allocate(nullptr, input.size, nullptr);
    if (!buffer) {
        throw std::bad_alloc();
    }
    gst_buffer_fill(buffer, 0, input.data.get(), input.size);
    GST_BUFFER_PTS(buffer) = std::chrono::duration_cast<std::chrono::nanoseconds>(input.timestamp - s.origin).count();
    // push_buffer 接管引用；后续错误不能再次释放 buffer。
    if (gst_app_src_push_buffer(GST_APP_SRC(s.source), buffer) != GST_FLOW_OK) {
        s.check_bus();
        throw std::runtime_error("appsrc rejected JPEG frame");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.config.timeout_ms);
    while (!s.stop) {
        Sample sample{gst_app_sink_try_pull_sample(GST_APP_SINK(s.sink), 20 * GST_MSECOND)};
        s.check_bus();
        if (sample.value) {
            GstVideoInfo info{};
            if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample.value)) ||
                GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
                GST_VIDEO_INFO_WIDTH(&info) != s.width || GST_VIDEO_INFO_HEIGHT(&info) != s.height) {
                throw std::runtime_error("decoder output is not the requested NV12 image");
            }
            MappedFrame mapped;
            mapped.mapped = gst_video_frame_map(&mapped.value, &info,
                                                gst_sample_get_buffer(sample.value), GST_MAP_READ);
            if (!mapped.mapped) {
                throw std::runtime_error("cannot map decoder output");
            }
            const auto y_size = static_cast<std::size_t>(s.width) * s.height;
            const auto size = y_size + y_size / 2;
            auto data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[size]);
            // GstVideoFrame 使用实际 VideoMeta 布局，逐行去除硬件对齐填充。
            for (unsigned plane = 0; plane < 2; ++plane) {
                const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped.value, plane);
                if (stride < s.width) {
                    throw std::runtime_error("invalid NV12 plane stride");
                }
                const auto* src = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped.value, plane));
                auto* dst = data.get() + (plane == 0 ? 0 : y_size);
                const int rows = plane == 0 ? s.height : s.height / 2;
                for (int row = 0; row < rows; ++row) {
                    std::memcpy(dst + static_cast<std::size_t>(row) * s.width,
                                src + static_cast<std::size_t>(row) * stride, s.width);
                }
            }
            camera::VideoFrame result = input;
            result.format = camera::PixelFormat::NV12;
            result.stride = s.width;
            result.data = std::move(data);
            result.size = size;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // 超时后不再提交下一帧，避免迟到的输出与下一帧元数据错误配对。
            throw std::runtime_error("hardware JPEG decode timed out without an output frame");
        }
    }
    return std::nullopt;
}

void GstVideoPipeline::request_stop() noexcept { impl_->stop = true; }
void GstVideoPipeline::close() noexcept {
    auto& s = *impl_;
    if (s.pipeline) {
        gst_element_set_state(s.pipeline, GST_STATE_NULL);
        gst_element_get_state(s.pipeline, nullptr, nullptr, 2 * GST_SECOND);
    }
    if (s.bus) {
        gst_object_unref(s.bus);
    }
    if (s.sink) {
        gst_object_unref(s.sink);
    }
    if (s.source) {
        gst_object_unref(s.source);
    }
    if (s.pipeline) {
        gst_object_unref(s.pipeline);
    }
    s.bus = nullptr;
    s.source = s.sink = s.pipeline = nullptr;
    s.width = s.height = 0;
}
} // namespace rkmon::video
