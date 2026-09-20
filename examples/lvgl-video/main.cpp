#include <lvgl.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <ctime>
#include <SDL.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>

namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopping = 0;
void signal_stop(int) { stopping = 1; }
std::uint32_t ticks() { return SDL_GetTicks(); }
struct Screen {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(1024 * 600);
    unsigned presents{};
    ~Screen() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* data) {
    auto& s = *static_cast<Screen*>(lv_display_get_user_data(display));
    const int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y)
        std::memcpy(s.pixels.data() + y * 1024 + area->x1,
                    data + (y - area->y1) * width * 4, width * 4);
    if (lv_display_flush_is_last(display)) {
        SDL_UpdateTexture(s.texture, nullptr, s.pixels.data(), 1024 * 4);
        SDL_RenderClear(s.renderer);
        SDL_RenderCopy(s.renderer, s.texture, nullptr, nullptr);
        SDL_RenderPresent(s.renderer);
        ++s.presents;
    }
    lv_display_flush_ready(display);
}
lv_obj_t* label(const char* text, int x, int y, const lv_font_t* font) {
    auto* obj = lv_label_create(lv_screen_active());
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe4edf5), 0);
    return obj;
}
struct Pipeline {
    GstElement* pipeline{};
    GstAppSink* sink{};
    ~Pipeline() {
        if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
        if (sink) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
    }
};
}
int main(int argc, char** argv) {
    try {
        int seconds = 60;
        std::string capture;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "lvgl-video-probe [--seconds N] [--capture FILE.bmp]\n"
                             "Fullscreen LVGL / local RTSP MPP test; Escape exits.\n";
                return 0;
            }
            if ((arg == "--seconds" || arg == "--capture") && i + 1 < argc) {
                if (arg == "--seconds") seconds = std::stoi(argv[++i]);
                else capture = argv[++i];
            } else throw std::runtime_error("invalid arguments");
        }
        if (seconds < 1 || seconds > 3600) throw std::runtime_error("seconds must be 1..3600");
        std::signal(SIGINT, signal_stop); std::signal(SIGTERM, signal_stop);
        gst_init(nullptr, nullptr);
        Screen s;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) throw std::runtime_error(SDL_GetError());
        s.window = SDL_CreateWindow("RKMon LVGL performance test", SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, 1024, 600, SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (!s.window) throw std::runtime_error(SDL_GetError());
        s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_ACCELERATED);
        if (!s.renderer) throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(s.renderer, 1024, 600);
        SDL_RendererInfo ri{}; SDL_GetRendererInfo(s.renderer, &ri);
        std::cout << "SDL backend=" << SDL_GetCurrentVideoDriver() << " renderer=" << ri.name << std::endl;
        s.texture = SDL_CreateTexture(s.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 1024, 600);
        if (!s.texture) throw std::runtime_error(SDL_GetError());
        SDL_ShowCursor(SDL_DISABLE);
        lv_init(); lv_tick_set_cb(ticks);
        auto* display = lv_display_create(1024, 600);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
        std::vector<std::uint32_t> draw(1024 * 100);
        lv_display_set_buffers(display, draw.data(), nullptr, draw.size() * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_user_data(display, &s); lv_display_set_flush_cb(display, flush);
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x111b28), 0);
        label("LOCAL CAMERA", 24, 24, &lv_font_montserrat_20);
        auto* clock = label("--:--:--", 786, 32, &lv_font_montserrat_32);
        label("LVGL VIDEO TEST", 786, 100, &lv_font_montserrat_20);
        auto* stats = label("Waiting for video", 24, 545, &lv_font_montserrat_20);
        auto* img = lv_image_create(lv_screen_active()); lv_obj_set_pos(img, 0, 84);
        std::vector<std::uint8_t> pixels(768 * 432 * 4);
        lv_image_dsc_t dsc{};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC; dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        dsc.header.w = 768; dsc.header.h = 432; dsc.header.stride = 768 * 4;
        dsc.data_size = pixels.size(); dsc.data = pixels.data();
        lv_image_set_src(img, &dsc);
        Pipeline p;
        GError* error = nullptr;
        p.pipeline = gst_parse_launch("rtspsrc location=rtsp://127.0.0.1:8554/camera protocols=tcp latency=0 "
            "! rtph264depay ! h264parse ! mppvideodec width=768 height=432 format=BGRx "
            "! appsink name=frames sync=false max-buffers=1 drop=true wait-on-eos=false", &error);
        if (error) { std::string msg = error->message; g_error_free(error); throw std::runtime_error(msg); }
        if (!p.pipeline) throw std::runtime_error("pipeline construction failed");
        p.sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(p.pipeline), "frames"));
        if (gst_element_set_state(p.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("pipeline startup failed");
        const auto start = Clock::now();
        auto first = start, last = start, next_clock = start;
        unsigned frames = 0, stalls = 0;
        std::vector<double> render_ms, gaps_ms;
        while (!stopping && Clock::now() - start < std::chrono::seconds(seconds)) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) stopping = 1;
            auto* sample = gst_app_sink_try_pull_sample(p.sink, 0);
            if (sample) {
                const auto began = Clock::now();
                GstVideoInfo info{}; GstVideoFrame frame{};
                if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
                    GST_VIDEO_INFO_WIDTH(&info) != 768 || GST_VIDEO_INFO_HEIGHT(&info) != 432 ||
                    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRx ||
                    !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
                    gst_sample_unref(sample); throw std::runtime_error("unexpected video format / map failure");
                }
                const auto* src = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                for (int y = 0; y < 432; ++y)
                    std::memcpy(pixels.data() + y * 768 * 4, src + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), 768 * 4);
                gst_video_frame_unmap(&frame); gst_sample_unref(sample);
                lv_image_cache_drop(&dsc); lv_obj_invalidate(img);
                lv_refr_now(display);
                auto now = Clock::now();
                render_ms.push_back(std::chrono::duration<double, std::milli>(now - began).count());
                if (!frames) first = now;
                else {
                    double gap = std::chrono::duration<double, std::milli>(now - last).count();
                    gaps_ms.push_back(gap); if (gap > 100) ++stalls;
                }
                last = now; ++frames;
            }
            auto now = Clock::now();
            if (now >= next_clock) {
                std::time_t t = std::time(nullptr); char buf[32]; std::tm tm{}; localtime_r(&t, &tm);
                std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm); lv_label_set_text(clock, buf);
                lv_label_set_text_fmt(stats, "768 x 432 | %u frames | %.1f fps", frames,
                    frames > 1 ? (frames - 1) / std::chrono::duration<double>(last - first).count() : 0);
                next_clock = now + std::chrono::seconds(1);
            }
            lv_timer_handler();
            auto* bus = gst_element_get_bus(p.pipeline);
            auto* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            gst_object_unref(bus);
            if (message) {
                std::string reason = "unexpected video EOS";
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError* ge = nullptr; gchar* debug = nullptr; gst_message_parse_error(message, &ge, &debug);
                    reason = ge->message; g_error_free(ge); g_free(debug);
                }
                gst_message_unref(message); throw std::runtime_error(reason);
            }
            SDL_Delay(1);
        }
        if (!capture.empty()) {
            auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(s.pixels.data(), 1024, 600, 32, 4096, SDL_PIXELFORMAT_ARGB8888);
            if (!surface || SDL_SaveBMP(surface, capture.c_str())) throw std::runtime_error(SDL_GetError());
            SDL_FreeSurface(surface);
        }
        auto percentile = [](std::vector<double> v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end()); return v[static_cast<std::size_t>((v.size() - 1) * q)];
        };
        rusage ru{}; getrusage(RUSAGE_SELF, &ru);
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        std::cout << "RESULT frames=" << frames << " seconds=" << elapsed
                  << " fps=" << (frames > 1 ? (frames - 1) / std::chrono::duration<double>(last - first).count() : 0)
                  << " cpu_percent=" << cpu / elapsed * 100 << " peak_rss_kib=" << ru.ru_maxrss
                  << " render_p50_ms=" << percentile(render_ms, .5) << " render_p95_ms=" << percentile(render_ms, .95)
                  << " gap_p95_ms=" << percentile(gaps_ms, .95) << " gaps_over_100ms=" << stalls
                  << " presents=" << s.presents << std::endl;
        lv_display_delete(display); lv_deinit();
        return frames > 0 ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
