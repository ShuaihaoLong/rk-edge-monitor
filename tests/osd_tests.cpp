#include "media/osd.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
}
int main() {
    try {
        rkmon::camera::VideoFrame frame;
        frame.received_at = std::chrono::system_clock::time_point{std::chrono::seconds(1672531200)};
        const auto text = rkmon::video::osd_text(frame, "Asia/Shanghai");
        check(text == "2023-01-01 08:00:00 UTC+0800", "OSD must use original receive date in specified timezone");
        check(rkmon::video::osd_text(frame, "UTC") == "2023-01-01 00:00:00 UTC+0000", "UTC conversion");
        frame.received_at = {};
        check(rkmon::video::osd_text(frame, "UTC") == "---- -- -- --:--:--", "unknown time must not show current time");

        for (const auto width : {2, 16, 320, 1920}) {
            const int height = width == 1920 ? 1080 : width;
            const int stride = width + 32;
            const auto y_size = static_cast<std::size_t>(stride) * height;
            const auto uv_size = y_size / 2;
            std::vector<std::uint8_t> y(y_size + 32, 90), uv(uv_size + 32, 70);
            rkmon::video::draw_osd(y.data(), stride, uv.data(), stride, width, height, text);
            check(std::all_of(y.begin() + y_size, y.end(), [](auto p) { return p == 90; }), "Y overrun");
            check(std::all_of(uv.begin() + uv_size, uv.end(), [](auto p) { return p == 70; }), "UV overrun");
            for (int row = 0; row < height; ++row)
                for (int col = width; col < stride; ++col)
                    check(y[static_cast<std::size_t>(row) * stride + col] == 90, "Y stride padding overwritten");
            for (int row = 0; row < height / 2; ++row)
                for (int col = width; col < stride; ++col)
                    check(uv[static_cast<std::size_t>(row) * stride + col] == 70, "UV stride padding overwritten");
            if (width >= 320) {
                check(std::find(y.begin(), y.end(), 235) != y.end(), "text missing");
                check(y[(height - 1) * stride] == 90 && uv[(height / 2 - 1) * stride] == 70,
                      "pixels outside OSD modified");
            }
        }
        std::cout << "OSD date, timezone, NV12 bounds and padding tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
