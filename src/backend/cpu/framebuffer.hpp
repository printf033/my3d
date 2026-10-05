#pragma once

// my3d::backend::cpu —— 软件光栅器的颜色 / 深度缓冲。
//
// 存**线性** RGB 浮点，不存 8 位 sRGB。两条理由：
//   - 着色必须在线性空间做，8 位量化放在着色之前会先丢掉暗部层次；
//   - 曝光与 tonemap 要在着色之后、量化之前做，需要 HDR 余量。
// sRGB 编码只发生在**写出时**，且集中在 encodeSrgb8 一个函数里。
// 散落在各处的 gamma 处理是画面发灰最常见的来源。
//
// 深度用 float 存 NDC z ∈ [-1, 1]（GL 约定，见 core/math.hpp 的 perspective）。
// 约定：**z 越小越近**，深度测试即 z < depth。
//
// 为什么不存 view-space 深度：z_ndc 是 1/w_clip 的仿射函数，而 1/w 在屏幕
// 空间是线性可插值的，所以 z_ndc 可以直接按屏幕空间重心**线性**插值，不需要
// 透视校正。换来一份更少也更难写错的算术。

#include "asset/image_data.hpp"
#include "core/math.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace my3d::backend::cpu
{

class Framebuffer
{
public:
    Framebuffer() = default;
    Framebuffer(uint32_t width, uint32_t height) { resize(width, height); }

    // 返回 true 表示尺寸**确实变了**。调用方据此决定是否重算相机 aspect、
    // 重建终端输出网格 —— 无谓的重建会让字符画面每帧闪烁。
    bool resize(uint32_t width, uint32_t height)
    {
        if (width == width_ && height == height_)
            return false;

        width_ = width;
        height_ = height;
        const size_t n = static_cast<size_t>(width) * height;
        colors_.assign(n, math::Vec3{0.0f, 0.0f, 0.0f});
        depths_.assign(n, 1.0f);

        // 尺寸变化后旧的深度值全部失效，必须重置为最远（1.0），
        // 否则新画面会被上一帧残留的深度挡住。
        return true;
    }

    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    size_t pixelCount() const noexcept { return colors_.size(); }
    bool empty() const noexcept { return width_ == 0 || height_ == 0; }

    bool contains(int32_t x, int32_t y) const noexcept
    {
        return x >= 0 && y >= 0 && static_cast<uint32_t>(x) < width_ &&
               static_cast<uint32_t>(y) < height_;
    }

    // color 为线性 RGB；depth 默认 1.0（NDC 最远）。
    void clear(const math::Vec3 &color, float depth = 1.0f)
    {
        std::fill(colors_.begin(), colors_.end(), color);
        std::fill(depths_.begin(), depths_.end(), depth);
    }

    void setColor(uint32_t x, uint32_t y, const math::Vec3 &linearRgb) noexcept
    {
        colors_[index(x, y)] = linearRgb;
    }

    const math::Vec3 &color(uint32_t x, uint32_t y) const noexcept
    {
        return colors_[index(x, y)];
    }

    float depth(uint32_t x, uint32_t y) const noexcept { return depths_[index(x, y)]; }

    void setDepth(uint32_t x, uint32_t y, float ndcZ) noexcept
    {
        depths_[index(x, y)] = ndcZ;
    }

    // z 更小即更近。注意：数值相等时**不**通过。重叠共面几何因此只画先到的那个，
    // 这是有意的 —— 用 <= 会让后画的每个共面三角形都重写颜色，浪费且不稳定。
    bool depthTest(uint32_t x, uint32_t y, float ndcZ) const noexcept
    {
        return ndcZ < depths_[index(x, y)];
    }

    // 感知亮度：先做 sRGB 编码再按 Rec.709 加权。
    //
    // 顺序很关键。终端把收到的字节当作 sRGB 显示，而线性值在暗部被极度压缩：
    // 线性 0.02 与 0.05 在屏幕上差得很远。直接对线性值加权会让暗部字符层次
    // 糊成一团。所以这里编码后再加权，让字符 ramp 的分配接近人眼所见的亮度。
    float luminance(uint32_t x, uint32_t y) const noexcept
    {
        const math::Vec3 &c = colors_[index(x, y)];
        return 0.2126f * linearToSrgb(c.x) + 0.7152f * linearToSrgb(c.y) +
               0.0722f * linearToSrgb(c.z);
    }

    // 抽成一张单通道浮点图（行优先，长度 w*h），供 ASCII 后端算 Sobel 邻域。
    // 做成函数而不是让 ASCII 后端自己遍历，是为了让「加权系数」只有一处定义。
    void extractLuminance(std::vector<float> &out) const
    {
        out.resize(colors_.size());
        for (size_t i = 0; i < colors_.size(); ++i)
        {
            const math::Vec3 &c = colors_[i];
            out[i] = 0.2126f * linearToSrgb(c.x) + 0.7152f * linearToSrgb(c.y) +
                     0.0722f * linearToSrgb(c.z);
        }
    }

    // 复用 asset 层的唯一定义，只补上两件它不管的事：
    //   - clamp：asset 版对 > 1 的 HDR 输入会返回 > 1，转 8 位时会回绕成一个
    //     极暗的值，而不是饱和到白。这是过曝区域突然变黑的经典成因。
    static float linearToSrgb(float c) noexcept
    {
        return asset::linearToSrgb(std::clamp(c, 0.0f, 1.0f));
    }

    static uint8_t encodeSrgb8(float linear) noexcept
    {
        return static_cast<uint8_t>(linearToSrgb(linear) * 255.0f + 0.5f);
    }

    // 转成 RGBA8 图像（colorSpace = SRGB，与 8 位量化的实际含义一致）。
    void toImage(asset::ImageData &out) const
    {
        out.width = width_;
        out.height = height_;
        out.colorSpace = asset::ColorSpace::SRGB;
        out.pixels.assign(colors_.size() * 4, 0);

        for (size_t i = 0; i < colors_.size(); ++i)
        {
            const math::Vec3 &c = colors_[i];
            out.pixels[i * 4 + 0] = encodeSrgb8(c.x);
            out.pixels[i * 4 + 1] = encodeSrgb8(c.y);
            out.pixels[i * 4 + 2] = encodeSrgb8(c.z);
            out.pixels[i * 4 + 3] = 255;
        }
    }

    // 光栅器批处理用。
    math::Vec3 *colorData() noexcept { return colors_.data(); }
    float *depthData() noexcept { return depths_.data(); }
    const math::Vec3 *colorData() const noexcept { return colors_.data(); }
    const float *depthData() const noexcept { return depths_.data(); }

private:
    size_t index(uint32_t x, uint32_t y) const noexcept
    {
        return static_cast<size_t>(y) * width_ + x;
    }

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<math::Vec3> colors_;
    std::vector<float> depths_;
};

} // namespace my3d::backend::cpu
