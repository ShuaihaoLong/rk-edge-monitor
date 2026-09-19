#include "media/osd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace rkmon::video {
namespace {
using Glyph = std::array<std::uint8_t, 7>;
Glyph glyph(char c) noexcept {
    switch (c) {
    case '0': return {14,17,19,21,25,17,14};
    case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31};
    case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2};
    case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14};
    case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14};
    case '9': return {14,17,17,15,1,1,14};
    case '-': return {0,0,0,31,0,0,0};
    case '+': return {0,4,4,31,4,4,0};
    case ':': return {0,4,4,0,4,4,0};
    case '.': return {0,0,0,0,0,4,4};
    case 'A': return {14,17,17,31,17,17,17};
    case 'C': return {14,17,16,16,16,17,14};
    case 'F': return {31,16,16,30,16,16,16};
    case 'P': return {30,17,17,30,16,16,16};
    case 'S': return {15,16,16,14,1,1,30};
    case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14};
    default: return {};
    }
}
}

std::array<std::string, 2> osd_text(const camera::VideoFrame& frame, const std::string& timezone) {
    if (timezone != "Asia/Shanghai" && timezone != "UTC" && timezone != "local")
        throw std::invalid_argument("unsupported OSD timezone");
    std::string date = "---- -- -- --:--:--";
    if (frame.received_at != std::chrono::system_clock::time_point{}) {
        const auto shifted = frame.received_at + (timezone == "Asia/Shanghai" ? std::chrono::hours(8) : std::chrono::hours(0));
        const auto seconds = std::chrono::system_clock::to_time_t(shifted);
        std::tm calendar{};
        auto* converted = timezone == "local" ? localtime_r(&seconds, &calendar) : gmtime_r(&seconds, &calendar);
        char text[64]{};
        if (converted && std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &calendar)) {
            date = text;
            if (timezone == "local") {
                char zone[16]{};
                if (std::strftime(zone, sizeof(zone), "%z", &calendar)) date += std::string(" UTC") + zone;
            } else {
                date += timezone == "Asia/Shanghai" ? " UTC+0800" : " UTC+0000";
            }
        }
    }
    std::string fps = "CAP FPS --";
    if (std::isfinite(frame.capture_fps) && frame.capture_fps > 0 && frame.capture_fps <= 1000) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "CAP FPS " << std::fixed << std::setprecision(1) << frame.capture_fps;
        fps = out.str();
    }
    return {std::move(date), std::move(fps)};
}

void draw_osd(std::uint8_t* y, std::size_t y_stride, std::uint8_t* uv, std::size_t uv_stride,
              int width, int height, const std::array<std::string, 2>& lines) noexcept {
    if (!y || !uv || width < 2 || height < 2 || width % 2 || height % 2 ||
        y_stride < static_cast<std::size_t>(width) || uv_stride < static_cast<std::size_t>(width)) return;
    const auto columns = std::min<std::size_t>(64, std::max(lines[0].size(), lines[1].size()));
    const int scale = std::max(1, std::min({6, height / 240, width / static_cast<int>(columns * 6 + 12)}));
    const int left = std::min(4 * scale, width - 2) & ~1;
    const int top = std::min(4 * scale, height - 2) & ~1;
    const int right = std::min(width, left + static_cast<int>(columns * 6 + 4) * scale + 1) & ~1;
    const int bottom = std::min(height, top + 22 * scale + 1) & ~1;
    for (int row = top; row < bottom; ++row)
        for (int col = left; col < right; ++col) {
            auto& pixel = y[static_cast<std::size_t>(row) * y_stride + col];
            pixel = static_cast<std::uint8_t>(16 + pixel / 4);
        }
    for (int row = top / 2; row < bottom / 2; ++row)
        std::fill(uv + static_cast<std::size_t>(row) * uv_stride + left,
                  uv + static_cast<std::size_t>(row) * uv_stride + right, 128);
    for (int line = 0; line < 2; ++line) {
        const auto count = std::min(columns, lines[line].size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto bitmap = glyph(lines[line][i]);
            for (int gy = 0; gy < 7 * scale; ++gy) {
                const int row = top + 2 * scale + line * 10 * scale + gy;
                if (row >= bottom) break;
                for (int gx = 0; gx < 5 * scale; ++gx) {
                    const int col = left + (2 + static_cast<int>(i) * 6) * scale + gx;
                    if (col >= right) break;
                    if (bitmap[gy / scale] & (1u << (4 - gx / scale)))
                        y[static_cast<std::size_t>(row) * y_stride + col] = 235;
                }
            }
        }
    }
}
} // namespace rkmon::video
