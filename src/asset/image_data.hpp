#pragma once

// 引擎自有的图像数据。
//
// 当前只有 RGBA8 一种布局（stb_image 一律解码为 4 通道）。
// 将来接入 KTX（IBL 的 RGB16F 环境贴图）时在这里扩展 format 字段 ——
// 但只有真正有后端实现时才加值，不留「声明了却没人实现」的格式。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace my3d::asset
{

// 颜色贴图（baseColor / emissive）存 sRGB 编码，数据贴图（normal / roughness /
// metallic / AO）存线性值。旧代码用 filament::Texture::InternalFormat::SRGB8_A8
// 与 RGBA8 区分，这里是同一件事的引擎表达。
enum class ColorSpace : uint8_t
{
    Linear,
    SRGB
};

inline float srgbToLinear(float c) noexcept
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c) noexcept
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct ImageData
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    ColorSpace colorSpace = ColorSpace::Linear;
    std::vector<uint8_t> pixels;   // RGBA8 交错，行优先，第 0 行在图像顶部

    bool valid() const noexcept
    {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * height * 4;
    }

    size_t byteSize() const noexcept { return pixels.size(); }

    const uint8_t *texel(uint32_t x, uint32_t y) const noexcept
    {
        return pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
    }

    // 双线性采样，clamp-to-edge。
    // UV 原点在左下（glTF / assimp 约定），而像素第 0 行在顶部，故 v 要翻转。
    void fetchRGBA(float u, float v, uint8_t out[4]) const noexcept
    {
        out[0] = out[1] = out[2] = out[3] = 255;
        if (!valid())
            return;

        const float fx = u * static_cast<float>(width) - 0.5f;
        const float fy = (1.0f - v) * static_cast<float>(height) - 0.5f;
        const int ix = static_cast<int>(std::floor(fx));
        const int iy = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(ix);
        const float ty = fy - static_cast<float>(iy);

        auto clampIndex = [](int a, int n) noexcept
        {
            return a < 0 ? 0 : (a >= n ? n - 1 : a);
        };
        const uint32_t x0 = static_cast<uint32_t>(clampIndex(ix, static_cast<int>(width)));
        const uint32_t x1 = static_cast<uint32_t>(clampIndex(ix + 1, static_cast<int>(width)));
        const uint32_t y0 = static_cast<uint32_t>(clampIndex(iy, static_cast<int>(height)));
        const uint32_t y1 = static_cast<uint32_t>(clampIndex(iy + 1, static_cast<int>(height)));

        for (int c = 0; c < 4; ++c)
        {
            const float v00 = texel(x0, y0)[c];
            const float v10 = texel(x1, y0)[c];
            const float v01 = texel(x0, y1)[c];
            const float v11 = texel(x1, y1)[c];
            const float top = v00 + (v10 - v00) * tx;
            const float bot = v01 + (v11 - v01) * tx;
            out[c] = static_cast<uint8_t>(std::clamp(top + (bot - top) * ty + 0.5f, 0.0f, 255.0f));
        }
    }
};

using ImagePtr = std::shared_ptr<const ImageData>;

} // namespace my3d::asset
