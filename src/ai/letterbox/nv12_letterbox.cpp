#include "nv12_letterbox.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "media/nv12.hpp"

namespace rkmon::ai {
LetterboxGeometry nv12_letterbox_geometry(const camera::VideoFrame& f, int size) {
    if (size <= 0 || size > 4096)
        throw std::invalid_argument("invalid model image size");
    media::validate_nv12(f);
    const float scale = std::min(float(size) / f.width, float(size) / f.height);
    const int w = std::max(1, int(std::round(f.width * scale))),
              h = std::max(1, int(std::round(f.height * scale)));
    return {scale, w, h, (size - w) / 2, (size - h) / 2};
}

ModelImage nv12_letterbox(const camera::VideoFrame& f, int size) {
    ModelImage out;
    nv12_letterbox(f, out, size);
    return out;
}

void nv12_letterbox(const camera::VideoFrame& f, ModelImage& out, int size) {
    const auto geometry = nv12_letterbox_geometry(f, size);
    const auto w = geometry.width, h = geometry.height;
    out.scale = geometry.scale;
    out.x_pad = geometry.x_pad;
    out.y_pad = geometry.y_pad;
    out.rgb.resize(static_cast<std::size_t>(size) * size * 3);
    std::fill(out.rgb.begin(), out.rgb.end(), 114);
    std::unique_ptr<media::DmaMapping> mapping;
    if (f.dma)
        mapping = std::make_unique<media::DmaMapping>(f.dma);
    const auto* yplane = mapping ? mapping->data() : f.data.get();
    const auto* uv = yplane + media::nv12_uv_offset(f);
    auto sample = [](const std::uint8_t* p, int width, int height, float x, float y, int step,
                     int offset, int stride) {
        x = std::clamp(x, 0.0f, float(width - 1));
        y = std::clamp(y, 0.0f, float(height - 1));
        const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, width - 1),
                  y1 = std::min(y0 + 1, height - 1);
        const float dx = x - x0, dy = y - y0;
        auto at = [&](int a, int b) {
            return p[b * stride + a * step + offset];
        };
        return (1 - dy) * ((1 - dx) * at(x0, y0) + dx * at(x1, y0)) +
               dy * ((1 - dx) * at(x0, y1) + dx * at(x1, y1));
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float sx = (x + 0.5f) * f.width / w - 0.5f, sy = (y + 0.5f) * f.height / h - 0.5f;
            const float l =
                1.164383f * (sample(yplane, f.width, f.height, sx, sy, 1, 0, f.stride) - 16);
            const float u =
                sample(uv, f.width / 2, f.height / 2, sx / 2, sy / 2, 2, 0, f.stride) - 128;
            const float v =
                sample(uv, f.width / 2, f.height / 2, sx / 2, sy / 2, 2, 1, f.stride) - 128;
            auto* dst = out.rgb.data() + ((y + out.y_pad) * size + x + out.x_pad) * 3;
            dst[0] = std::clamp(int(std::round(l + 1.596027f * v)), 0, 255);
            dst[1] = std::clamp(int(std::round(l - 0.391762f * u - 0.812968f * v)), 0, 255);
            dst[2] = std::clamp(int(std::round(l + 2.017232f * u)), 0, 255);
        }
}
}
