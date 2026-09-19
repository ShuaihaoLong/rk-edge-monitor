#include "media/gstreamer/gst_video_pipeline.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include "media/nv12.hpp"
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rkmon::video {
namespace {
// DMA 输出帧持有 GstBuffer 引用，最后一个消费者释放后才归还解码池。
struct Sample {
    GstSample* value;
    ~Sample() { if (value) gst_sample_unref(value); }
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
        // 厂商插件须显式设置 format；部分版本提供 DMA 内存却不在输出 caps 标记 DMABuf。
        // caps 接受其特征差异，实际输出仍严格检查 GstDmaBufMemory。
        s.pipeline = gst_parse_launch(
            "appsrc name=input is-live=true format=time block=false max-buffers=1 max-bytes=0 "
            "! jpegparse ! mppjpegdec format=NV12 dma-feature=true "
            "! video/x-raw(ANY),format=NV12 "
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
            auto* decoded=gst_sample_get_buffer(sample.value);
            auto* meta=gst_buffer_get_video_meta(decoded);
            if(gst_buffer_n_memory(decoded)!=1 || !meta || meta->n_planes!=2)
                throw std::runtime_error("decoder requires single DMA buffer and NV12 video metadata");
            auto* memory=gst_buffer_peek_memory(decoded,0);
            gsize offset=0,maxsize=0;
            const auto bytes=gst_memory_get_sizes(memory,&offset,&maxsize);
            if(!gst_is_dmabuf_memory(memory) || offset!=0 || meta->offset[0]!=0 ||
               meta->stride[0]<=0 || meta->stride[0]!=meta->stride[1] ||
               meta->offset[1]%static_cast<std::size_t>(meta->stride[0]))
                throw std::runtime_error("unsupported decoder DMA layout");
            std::shared_ptr<void> owner(gst_buffer_ref(decoded),[](void* ptr){gst_buffer_unref(static_cast<GstBuffer*>(ptr));});
            camera::VideoFrame result=input;
            result.format=camera::PixelFormat::NV12;
            result.stride=meta->stride[0];result.uv_offset=meta->offset[1];
            result.height_stride=result.uv_offset/result.stride;
            result.data.reset();result.size=bytes;
            result.dma=std::make_shared<media::DmaBuffer>(media::DmaBuffer{gst_dmabuf_memory_get_fd(memory),bytes,std::move(owner)});
            media::validate_nv12(result);
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
