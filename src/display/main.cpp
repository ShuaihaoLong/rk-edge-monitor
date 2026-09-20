#include <lvgl.h>
#include "video_reader.hpp"
#include "ad_reader.hpp"
#include "telemetry.hpp"
#include "app/config.hpp"
#include <json-c/json.h>
#include <fstream>
#include <memory>
#include <cmath>
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
    SDL_Rect viewport{0, 0, 1024, 600};
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(1024 * 600);
    unsigned presents{};
    bool failed{};
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
        if (SDL_UpdateTexture(s.texture, nullptr, s.pixels.data(), 1024 * 4) ||
            SDL_RenderClear(s.renderer) || SDL_RenderCopy(s.renderer, s.texture, nullptr, &s.viewport)) s.failed = true;
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

void update_weather(const std::string& path, lv_obj_t* place, lv_obj_t* weather, lv_obj_t* age) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() > 8192) {
        lv_label_set_text(place, "天气 · 合肥"); lv_label_set_text(weather, "等待联网");
        lv_label_set_text(age, "暂无天气数据"); return;
    }
    input.seekg(0); std::string text((std::istreambuf_iterator<char>(input)), {});
    std::unique_ptr<json_object, decltype(&json_object_put)> obj(json_tokener_parse(text.c_str()), json_object_put);
    if (!obj || !json_object_is_type(obj.get(), json_type_object)) {
        lv_label_set_text(weather, "天气数据不可用"); return;
    }
    auto str = [&](const char* key, const char* fallback) {
        json_object* v = nullptr;
        return json_object_object_get_ex(obj.get(), key, &v) && json_object_is_type(v, json_type_string)
            ? std::string(json_object_get_string(v)) : std::string(fallback);
    };
    auto num = [&](const char* key, double fallback) {
        json_object* v = nullptr;
        return json_object_object_get_ex(obj.get(), key, &v) &&
            (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double))
            ? json_object_get_double(v) : fallback;
    };
    const auto city = str("city", "合肥");
    lv_label_set_text_fmt(place, "天气 · %s", city.c_str());
    const double updated = num("updated_at", 0), temp = num("temperature_c", NAN);
    if (!updated || !std::isfinite(temp)) {
        lv_label_set_text(weather, "天气暂不可用"); lv_label_set_text(age, "联网后自动更新"); return;
    }
    lv_label_set_text_fmt(weather, "%.1f °C\n%s", temp, str("description", "").c_str());
    const auto elapsed = static_cast<long long>(std::time(nullptr) - updated);
    const bool cached = str("status", "offline") != "ok" || elapsed > 1800 || elapsed < -60;
    lv_label_set_text_fmt(age, "%s · %lld分钟前更新", cached ? "离线/缓存" : "已更新", std::max(0LL, elapsed / 60));
}

}
int main(int argc, char** argv) {
    try {
        int seconds = 0;
        std::string config = "/opt/rkmon/config/rkmon.ini";
        std::string settings = "/opt/rkmon/config/display.json";
        std::string capture;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "rkmon-display [--config FILE] [--settings FILE] [--seconds N] [--capture FILE.bmp]\n"
                             "Local monitor; Escape exits. --seconds 0 runs continuously.\n";
                return 0;
            }
            if ((arg == "--seconds" || arg == "--capture" || arg == "--config" || arg == "--settings") && i + 1 < argc) {
                if (arg == "--seconds") seconds = std::stoi(argv[++i]);
                else if (arg == "--config") config = argv[++i];
                else if (arg == "--settings") settings = argv[++i];
                else capture = argv[++i];
            } else throw std::runtime_error("invalid arguments");
        }
        if (seconds < 0 || seconds > 86400) throw std::runtime_error("seconds must be 0..86400");
        const auto runtime = rkmon::app::load_config(config);
        if (!runtime.stream) throw std::runtime_error("local display requires configured RTSP stream");
        std::unique_ptr<json_object, decltype(&json_object_put)> settings_json(json_object_from_file(settings.c_str()), json_object_put);
        if (!settings_json || !json_object_is_type(settings_json.get(), json_type_object))
            throw std::runtime_error("cannot load display settings");
        auto setting = [&](const char* key) {
            json_object* value = nullptr;
            if (!json_object_object_get_ex(settings_json.get(), key, &value) || !json_object_is_type(value, json_type_string))
                throw std::runtime_error(std::string("missing string setting: ") + key);
            return std::string(json_object_get_string(value));
        };
        const auto font_path = setting("font");
        const auto weather_path = setting("weather_cache");
        const auto ads_path = setting("ads_root");
        const auto timezone = setting("timezone");
        if (timezone.empty() || timezone.front() == '/' || timezone.find("..") != std::string::npos ||
            !std::ifstream("/usr/share/zoneinfo/" + timezone)) throw std::runtime_error("invalid timezone");
        setenv("TZ", timezone.c_str(), 1); tzset();
        std::signal(SIGINT, signal_stop); std::signal(SIGTERM, signal_stop);
        gst_init(nullptr, nullptr);
        Screen s;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) throw std::runtime_error(SDL_GetError());
        s.window = SDL_CreateWindow("RKMon 本地监控", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, 1024, 600, SDL_WINDOW_FULLSCREEN);
        if (!s.window) throw std::runtime_error(SDL_GetError());
        SDL_SetWindowBordered(s.window, SDL_FALSE);
        if (SDL_SetWindowFullscreen(s.window, SDL_WINDOW_FULLSCREEN))
            throw std::runtime_error(SDL_GetError());
        SDL_PumpEvents();
        s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_ACCELERATED);
        if (!s.renderer) throw std::runtime_error(SDL_GetError());
        int window_width = 0, window_height = 0;
        SDL_GetWindowSize(s.window, &window_width, &window_height);
        std::cout << "SDL window=" << window_width << "x" << window_height
                  << " flags=0x" << std::hex << SDL_GetWindowFlags(s.window) << std::dec << std::endl;
        int output_width = 0, output_height = 0;
        if (SDL_GetRendererOutputSize(s.renderer, &output_width, &output_height) ||
            output_width <= 0 || output_height <= 0) throw std::runtime_error(SDL_GetError());
        const double scale = std::min(output_width / 1024.0, output_height / 600.0);
        s.viewport.w = static_cast<int>(1024 * scale);
        s.viewport.h = static_cast<int>(600 * scale);
        s.viewport.x = (output_width - s.viewport.w) / 2;
        s.viewport.y = (output_height - s.viewport.h) / 2;
        std::cout << "SDL output=" << output_width << "x" << output_height
                  << " viewport=" << s.viewport.x << "," << s.viewport.y << " "
                  << s.viewport.w << "x" << s.viewport.h << std::endl;
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
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x2a211c), 0);
        auto* font = lv_freetype_font_create(font_path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20, LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!font) throw std::runtime_error("cannot load CJK font");
        auto* compact_font = lv_freetype_font_create(font_path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16, LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!compact_font) throw std::runtime_error("cannot load compact CJK font");
        label("RKMON", 24, 22, font);
        auto* clock = label("--:--:--", 790, 24, &lv_font_montserrat_32);
        auto* date = label("", 790, 72, font);
        auto* place = label("天气 · 合肥", 790, 125, font);
        auto* weather = label("等待联网", 790, 164, font);
        auto* weather_age = label("", 790, 234, compact_font);
        lv_obj_set_width(place, 220); lv_label_set_long_mode(place, LV_LABEL_LONG_DOT);
        lv_obj_set_width(weather, 220); lv_obj_set_width(weather_age, 220);
        label("当前环境:", 790, 314, font);
        auto* temperature = label("温度  --", 790, 358, font);
        auto* humidity = label("湿度  --", 790, 401, font);
        label("Open-Meteo / ipapi", 790, 560, &lv_font_montserrat_14);
        auto* stats = label("正在连接摄像头", 24, 548, font);
        auto* offline = label("视频连接中", 280, 282, font);
        auto* img = lv_image_create(lv_screen_active()); lv_obj_set_pos(img, 0, 84);
        std::vector<std::uint8_t> pixels(768 * 432 * 4);
        lv_image_dsc_t dsc{};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC; dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        dsc.header.w = 768; dsc.header.h = 432; dsc.header.stride = 768 * 4;
        dsc.data_size = pixels.size(); dsc.data = pixels.data();
        lv_image_set_src(img, &dsc);
        rkmon::display::VideoReader reader(runtime.stream->url);
        rkmon::display::AdReader ad_reader(ads_path);
        std::unique_ptr<rkmon::display::Telemetry> telemetry;
        if (runtime.mqtt && runtime.stm32)
            telemetry = std::make_unique<rkmon::display::Telemetry>(*runtime.mqtt, runtime.stm32->stale_timeout_ms);
        rkmon::display::VideoImage video;
        const auto start = Clock::now();
        auto first = start, last = start, next_clock = start;
        auto next_frame = start;
        constexpr auto frame_period = std::chrono::milliseconds(50);
        bool source_ad = false;
        unsigned frames = 0, stalls = 0;
        std::vector<double> render_ms, gaps_ms;
        while (!stopping && (!seconds || Clock::now() - start < std::chrono::seconds(seconds))) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) stopping = 1;
            const auto loop_now = Clock::now();
            bool has_video = false;
            if (loop_now >= next_frame) {
                const bool ad_mode = ad_reader.advertising();
                if (ad_mode != source_ad) {
                    source_ad = ad_mode;
                    reader.set_enabled(!ad_mode);
                    frames = 0;
                    stalls = 0;
                    first = last = loop_now;
                }
                has_video = source_ad ? ad_reader.take(video) : reader.take(video);
                next_frame = loop_now + frame_period;
            }
            if (has_video) {
                const auto began = Clock::now();
                pixels.swap(video.pixels);
                dsc.data = pixels.data();
                lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(offline, LV_OBJ_FLAG_HIDDEN);
                lv_image_set_src(img, &dsc);
                lv_image_cache_drop(&dsc); lv_obj_invalidate(img);
                lv_refr_now(display);
                auto now = Clock::now();
                if (seconds) render_ms.push_back(std::chrono::duration<double, std::milli>(now - began).count());
                if (!frames) first = now;
                else {
                    double gap = std::chrono::duration<double, std::milli>(now - last).count();
                    if (seconds) gaps_ms.push_back(gap);
                    if (gap > 100) ++stalls;
                }
                last = now; ++frames;
            }
            auto now = Clock::now();
            if (now >= next_clock) {
                std::time_t t = std::time(nullptr); char buf[32]; std::tm tm{}; localtime_r(&t, &tm);
                std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm); lv_label_set_text(clock, buf);
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm); lv_label_set_text(date, buf);
                const bool video_online = frames && now - last < std::chrono::seconds(2);
                if (!video_online) {
                    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(offline, LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(offline, source_ad ? "广告加载中" : "视频离线 · 正在重连");
                }
                if (source_ad)
                    lv_label_set_text(stats, video_online ? "广告播放  ·  本地视频" : "广告加载中");
                else
                    lv_label_set_text(stats, video_online ? "实时画面  ·  本机视频流" : reader.status().c_str());
                if (telemetry) {
                    const auto sensor = telemetry->snapshot();
                    if (sensor.online) {
                        lv_label_set_text_fmt(temperature, "温度  %.1f °C", sensor.temperature);
                        lv_label_set_text_fmt(humidity, "湿度  %.1f %%", sensor.humidity);
                    } else {
                        lv_label_set_text(temperature, "温度  --"); lv_label_set_text(humidity, "湿度  --");
                    }
                }
                update_weather(weather_path, place, weather, weather_age);
                next_clock = now + std::chrono::seconds(1);
            }
            lv_timer_handler();
            if (s.failed) throw std::runtime_error(SDL_GetError());
            SDL_Delay(5);
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
        lv_display_delete(display); lv_freetype_font_delete(font); lv_deinit();
        return seconds && !frames ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
