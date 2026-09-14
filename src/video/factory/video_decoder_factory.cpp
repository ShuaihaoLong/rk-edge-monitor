#include "video/interfaces/video_decoder.hpp"
#ifdef RKMON_WITH_GSTREAMER
#include "video/gstreamer/gst_video_pipeline.hpp"
#endif
#include <stdexcept>

namespace rkmon::video {
std::unique_ptr<IVideoDecoder> make_hardware_decoder(DecodeConfig config) {
#ifdef RKMON_WITH_GSTREAMER
    return std::make_unique<GstVideoPipeline>(config);
#else
    (void)config;
    throw std::runtime_error("hardware decoding requires RKMON_WITH_GSTREAMER=ON");
#endif
}
}
