#include "media/decoder.hpp"
#include "media/publisher.hpp"
#ifdef RKMON_WITH_GSTREAMER
#include "media/gstreamer/gst_video_pipeline.hpp"
#include "media/gstreamer/gst_rtsp_publisher.hpp"
#endif
#include <stdexcept>
#include <utility>

namespace rkmon::video {
std::unique_ptr<IVideoDecoder> make_hardware_decoder(DecodeConfig config) {
#ifdef RKMON_WITH_GSTREAMER
    return std::make_unique<GstVideoPipeline>(config);
#else
    (void)config;
    throw std::runtime_error("hardware decoding requires RKMON_WITH_GSTREAMER=ON");
#endif
}

std::unique_ptr<IVideoPublisher> make_hardware_publisher(StreamConfig config) {
#ifdef RKMON_WITH_GSTREAMER
    return std::make_unique<GstRtspPublisher>(std::move(config));
#else
    (void)config;
    throw std::runtime_error("hardware publishing requires RKMON_WITH_GSTREAMER=ON");
#endif
}
}
