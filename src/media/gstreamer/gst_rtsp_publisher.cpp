#include "media/gstreamer/gst_rtsp_publisher.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace rkmon::video {
namespace {
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
struct GstRtspPublisher::Impl {
    StreamConfig config;
    GstElement* pipeline{nullptr};
    GstElement* source{nullptr};
    GstBus* bus{nullptr};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> encoded{0};
    std::atomic<std::int64_t> last_encoded{0};
    bool submitted{false};
    GstVideoInfo info{};
    std::chrono::steady_clock::time_point origin{}, previous{};

    static GstPadProbeReturn on_encoded(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto& self = *static_cast<Impl*>(opaque);
        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
            ++self.encoded;
            self.last_encoded = now_ns();
        }
        return GST_PAD_PROBE_OK;
    }
};

GstRtspPublisher::GstRtspPublisher(StreamConfig config) : impl_(std::make_unique<Impl>()) {
    if (config.url.rfind("rtsp://", 0) != 0 || !config.fps || config.fps > 1000 ||
        !config.bitrate || config.bitrate > 100000000 || !config.gop || config.gop > 1000 ||
        !config.timeout_ms || config.timeout_ms > 60000) {
        throw std::invalid_argument("invalid RTSP publisher configuration");
    }
    impl_->config = std::move(config);
}
GstRtspPublisher::~GstRtspPublisher() { close(); }

void GstRtspPublisher::open() {
    auto& s = *impl_;
    if (s.pipeline) {
        throw std::logic_error("publisher already open");
    }
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        const std::string reason = error ? error->message : "GStreamer initialization failed";
        g_clear_error(&error);
        throw std::runtime_error(reason);
    }
    s.stop = false;
    s.encoded = 0;
    s.submitted = false;
    gst_video_info_init(&s.info);
    try {
        // 只在编码前丢弃过期原始帧。H.264 与 RTSP 直接相连，避免破坏参考帧依赖。
        s.pipeline = gst_parse_launch(
            "appsrc name=input is-live=true format=time block=false max-buffers=2 max-bytes=0 leaky-type=downstream "
            "! mpph264enc name=encoder profile=baseline header-mode=each-idr "
            "! h264parse name=parser config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! rtspclientsink name=publisher protocols=tcp latency=0 tcp-timeout=3000000", &error);
        if (error || !s.pipeline) {
            const std::string reason = error ? error->message : "cannot create publisher pipeline";
            g_clear_error(&error);
            throw std::runtime_error(reason);
        }
        s.source = gst_bin_get_by_name(GST_BIN(s.pipeline), "input");
        s.bus = gst_element_get_bus(s.pipeline);
        auto* encoder = gst_bin_get_by_name(GST_BIN(s.pipeline), "encoder");
        g_object_set(encoder, "bps", s.config.bitrate, "gop", static_cast<int>(s.config.gop), nullptr);
        gst_object_unref(encoder);
        auto* publisher = gst_bin_get_by_name(GST_BIN(s.pipeline), "publisher");
        // URL 作为属性设置，不能拼接进 parse_launch 字符串。
        g_object_set(publisher, "location", s.config.url.c_str(), nullptr);
        gst_object_unref(publisher);
        auto* parser = gst_bin_get_by_name(GST_BIN(s.pipeline), "parser");
        auto* pad = gst_element_get_static_pad(parser, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Impl::on_encoded, &s, nullptr);
        gst_object_unref(pad);
        gst_object_unref(parser);
        if (gst_element_set_state(s.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            check_health();
            throw std::runtime_error("cannot start publisher pipeline");
        }
    } catch (...) {
        close();
        throw;
    }
}

void GstRtspPublisher::check_health() {
    auto& s = *impl_;
    if (s.stop) return;
    if (!s.pipeline) throw std::logic_error("publisher is not open");
    auto* message = gst_bus_pop_filtered(s.bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (message) {
        std::string reason = "unexpected publisher EOS";
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            reason = error ? error->message : "GStreamer publisher failed";
            g_clear_error(&error);
            g_free(debug);
        }
        gst_message_unref(message);
        throw std::runtime_error(reason);
    }
    if (s.submitted && now_ns() - s.last_encoded.load() > static_cast<std::int64_t>(s.config.timeout_ms) * 1000000) {
        throw std::runtime_error("encoder stalled or input stream interrupted");
    }
}

void GstRtspPublisher::write(const camera::VideoFrame& frame) {
    auto& s = *impl_;
    if (s.stop) return;
    check_health();
    const auto width = frame.width;
    const auto height = frame.height;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || width % 2 || height % 2 ||
        frame.format != camera::PixelFormat::NV12 || frame.stride != static_cast<std::size_t>(width) ||
        !frame.data || frame.size != static_cast<std::size_t>(width) * height * 3 / 2) {
        throw std::invalid_argument("publisher requires tightly packed NV12 with even dimensions");
    }
    if (!s.submitted) {
        gst_video_info_set_format(&s.info, GST_VIDEO_FORMAT_NV12, width, height);
        s.info.fps_n = static_cast<int>(s.config.fps);
        s.info.fps_d = 1;
        auto* caps = gst_video_info_to_caps(&s.info);
        gst_app_src_set_caps(GST_APP_SRC(s.source), caps);
        gst_caps_unref(caps);
        s.origin = s.previous = frame.timestamp;
        s.last_encoded = now_ns();
        s.submitted = true;
    }
    if (width != GST_VIDEO_INFO_WIDTH(&s.info) || height != GST_VIDEO_INFO_HEIGHT(&s.info) ||
        frame.timestamp < s.previous) {
        throw std::runtime_error("publisher input format changed or timestamp regressed");
    }
    s.previous = frame.timestamp;
    auto* buffer = gst_buffer_new_allocate(nullptr, s.info.size, nullptr);
    if (!buffer) throw std::bad_alloc();
    GstMapInfo mapping{};
    if (!gst_buffer_map(buffer, &mapping, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        throw std::runtime_error("cannot map encoder input");
    }
    // 标准 GStreamer 布局可能包含行对齐，将独立 NV12 图像复制到协商布局。
    std::memset(mapping.data, 0, mapping.size);
    for (unsigned plane = 0; plane < 2; ++plane) {
        const auto* src = frame.data.get() + (plane ? static_cast<std::size_t>(width) * height : 0);
        auto* dst = mapping.data + s.info.offset[plane];
        const int rows = plane ? height / 2 : height;
        for (int row = 0; row < rows; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(row) * s.info.stride[plane],
                        src + static_cast<std::size_t>(row) * width, width);
        }
    }
    gst_buffer_unmap(buffer, &mapping);
    GST_BUFFER_PTS(buffer) = std::chrono::duration_cast<std::chrono::nanoseconds>(frame.timestamp - s.origin).count();
    GST_BUFFER_DURATION(buffer) = GST_SECOND / s.config.fps;
    // appsrc 接管引用；没有阻塞 push，停止请求不必等待网络发送。
    if (gst_app_src_push_buffer(GST_APP_SRC(s.source), buffer) != GST_FLOW_OK && !s.stop) {
        throw std::runtime_error("encoder input rejected");
    }
}
void GstRtspPublisher::request_stop() noexcept { impl_->stop = true; }
std::uint64_t GstRtspPublisher::encoded_frames() const noexcept { return impl_->encoded.load(); }
void GstRtspPublisher::close() noexcept {
    auto& s = *impl_;
    // 工作线程已退出；NULL 状态结束流线程后再销毁含 pad probe 的管线。
    if (s.pipeline) gst_element_set_state(s.pipeline, GST_STATE_NULL);
    if (s.bus) gst_object_unref(s.bus);
    if (s.source) gst_object_unref(s.source);
    if (s.pipeline) gst_object_unref(s.pipeline);
    s.bus = nullptr;
    s.source = s.pipeline = nullptr;
}
} // namespace rkmon::video
