#include "video_publisher.hpp"
#ifdef RKMON_WITH_GSTREAMER
#include "gst_rtsp_publisher.hpp"
#endif
#include <stdexcept>

namespace rkmon::video {
std::unique_ptr<IVideoPublisher> make_hardware_publisher(StreamConfig config) {
#ifdef RKMON_WITH_GSTREAMER
    return std::make_unique<GstRtspPublisher>(std::move(config));
#else
    (void)config;
    throw std::runtime_error("hardware publishing requires RKMON_WITH_GSTREAMER=ON");
#endif
}
}
