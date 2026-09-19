#include "media/gstreamer/gst_video_pipeline.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include "media/nv12.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
rkmon::camera::VideoFrame black_jpeg() {
    auto* pipeline = gst_parse_launch(
        "videotestsrc pattern=black num-buffers=1 ! video/x-raw,format=I420,width=320,height=240 "
        "! jpegenc ! appsink name=sink sync=false", nullptr);
    check(pipeline, "JPEG fixture pipeline unavailable");
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    rkmon::camera::VideoFrame input;
    if (sample) {
        auto* buffer = gst_sample_get_buffer(sample);
        input.size = gst_buffer_get_size(buffer);
        auto bytes = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[input.size]);
        gst_buffer_extract(buffer, 0, bytes.get(), input.size);
        input.data = std::move(bytes);
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    check(input.data != nullptr, "cannot generate JPEG fixture");
    input.width = 320;
    input.height = 240;
    input.sequence = 123;
    input.timestamp = std::chrono::steady_clock::now();
    input.received_at = std::chrono::system_clock::now();
    return input;
}
}
int main() {
    try {
        gst_init(nullptr, nullptr);
        auto input = black_jpeg();
        rkmon::video::GstVideoPipeline decoder({1000, 2});
        for (int round = 0; round < 2; ++round) {
            decoder.open();
            auto out = decoder.decode(input);
            check(out && out->sequence == 123 && out->timestamp == input.timestamp &&
                  out->received_at == input.received_at,
                  "source identity or receive metadata lost");
            rkmon::media::validate_nv12(*out);
            check(out->dma && !out->data,"decoder did not export DMA buffer");
            {
                rkmon::media::DmaMapping mapping(out->dma);
                for(int y=0;y<240;++y)for(int x=0;x<320;++x)
                    check(mapping.data()[y*out->stride+x]<=20,"black Y plane corrupted");
                for(int y=0;y<120;++y)for(int x=0;x<320;++x) {
                    const auto value=mapping.data()[out->uv_offset+y*out->stride+x];
                    check(value>=123 && value<=133,"neutral UV plane corrupted");
                }
            }
            decoder.close();
            rkmon::media::DmaMapping retained(out->dma);
            check(retained.data()[out->uv_offset]>=123,"DMA frame invalid after decoder close");
        }
        // 不完整 JPEG 不能被当作成功，必须在超时后向上报告错误。
        auto bad = input;
        bad.size = 4;
        decoder.open();
        bool failed = false;
        try { decoder.decode(bad); } catch (const std::runtime_error&) { failed = true; }
        check(failed, "truncated JPEG accepted");
        decoder.close();

        decoder.open();
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            decoder.request_stop();
        });
        const auto before = std::chrono::steady_clock::now();
        std::optional<rkmon::camera::VideoFrame> stopped;
        try { stopped = decoder.decode(bad); } catch (...) { stopper.join(); throw; }
        stopper.join();
        decoder.close();
        check(!stopped && std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500),
              "stop failed to interrupt appsink wait");
        std::cout << "hardware decoder tests passed: content, layout, identity, restart, timeout, stop\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
