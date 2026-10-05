#pragma once

// 图像处理：降采样、mip 链生成。
// 纯 CPU、零第三方依赖 —— CPU / ASCII 后端与 importer 共用同一份实现。

#include "image_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace my3d::asset
{

// 2x2 box filter 降采样一半（每边至少 1 像素）。
inline ImageData downsampleHalf(const ImageData &src) noexcept
{
    ImageData next;
    next.name = src.name;
    next.colorSpace = src.colorSpace;
    next.width = src.width > 1 ? src.width / 2 : 1;
    next.height = src.height > 1 ? src.height / 2 : 1;
    if (!src.valid())
        return next;

    next.pixels.resize(static_cast<size_t>(next.width) * next.height * 4);
    for (uint32_t y = 0; y < next.height; ++y)
    {
        for (uint32_t x = 0; x < next.width; ++x)
        {
            const uint32_t sx0 = std::min(x * 2, src.width - 1);
            const uint32_t sx1 = std::min(x * 2 + 1, src.width - 1);
            const uint32_t sy0 = std::min(y * 2, src.height - 1);
            const uint32_t sy1 = std::min(y * 2 + 1, src.height - 1);

            uint8_t *dst = next.pixels.data() + (static_cast<size_t>(y) * next.width + x) * 4;
            for (int c = 0; c < 4; ++c)
            {
                const int sum = src.texel(sx0, sy0)[c] + src.texel(sx1, sy0)[c] +
                                src.texel(sx0, sy1)[c] + src.texel(sx1, sy1)[c];
                dst[c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    return next;
}

// 逐级降采样到 1x1。maxLevels = 0 表示不设上限；返回的 level 0 是 src 的副本。
inline std::vector<ImageData> buildMipChain(const ImageData &src, uint32_t maxLevels = 0) noexcept
{
    std::vector<ImageData> chain;
    if (!src.valid())
        return chain;

    chain.push_back(src);
    while (chain.back().width > 1 || chain.back().height > 1)
    {
        if (maxLevels != 0 && chain.size() >= maxLevels)
            break;
        chain.push_back(downsampleHalf(chain.back()));
    }
    return chain;
}

// 反复半采样直到最长边不超过 maxSize。maxSize == 0 时原样返回。
inline ImagePtr downscaleToFit(ImagePtr src, uint32_t maxSize) noexcept
{
    if (!src || maxSize == 0)
        return src;
    while (src->valid() && (src->width > maxSize || src->height > maxSize))
        src = std::make_shared<const ImageData>(downsampleHalf(*src));
    return src;
}

// 按屏幕空间 UV 导数选 mip 层，层内双线性，层间线性插值。
inline void fetchRGBA_Lod(const std::vector<ImageData> &chain, float u, float v,
                          float duvPerPixel, uint8_t out[4]) noexcept
{
    if (chain.empty())
    {
        out[0] = out[1] = out[2] = out[3] = 255;
        return;
    }
    float lod = 0.0f;
    if (duvPerPixel > 0.0f && chain[0].width > 0)
    {
        const float texelsPerPixel = duvPerPixel * static_cast<float>(chain[0].width);
        lod = std::log2(std::max(texelsPerPixel, 1e-6f));
    }
    const int maxLod = static_cast<int>(chain.size()) - 1;
    const float clamped = std::clamp(lod, 0.0f, static_cast<float>(maxLod));
    const int lo = static_cast<int>(clamped);
    const int hi = std::min(lo + 1, maxLod);
    const float frac = clamped - static_cast<float>(lo);

    uint8_t a[4]{}, b[4]{};
    chain[static_cast<size_t>(lo)].fetchRGBA(u, v, a);
    if (hi == lo || frac <= 0.0f)
    {
        std::memcpy(out, a, 4);
        return;
    }
    chain[static_cast<size_t>(hi)].fetchRGBA(u, v, b);
    for (int c = 0; c < 4; ++c)
        out[c] = static_cast<uint8_t>(a[c] + (b[c] - a[c]) * frac);
}

} // namespace my3d::asset
