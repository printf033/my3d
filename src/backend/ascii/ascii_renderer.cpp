#include "backend/ascii/ascii_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace my3d::backend::ascii
{
namespace
{

// 两格在终端上看起来是否一样。
//
// level / isEdge 不参与比较：它们是 codePoint 的派生量，字形和颜色都相同却
// 重发一次，纯属白费带宽 —— 而带宽正是这个后端的瓶颈。
inline bool sameCell(const AsciiCell &a, const AsciiCell &b) noexcept
{
    return a.codePoint == b.codePoint && a.r == b.r && a.g == b.g && a.b == b.b;
}

inline bool sameColor(const AsciiCell &a, const AsciiCell &b) noexcept
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

void appendUtf8(std::string &out, char32_t codePoint)
{
    char buf[4];
    // charset::encodeUtf8 保证返回合法 UTF-8（非法码点 → U+FFFD），
    // 所以这里不需要检查 codePoint —— 半个字符会让整个终端行错位。
    const size_t n = encodeUtf8(codePoint, buf);
    out.append(buf, n);
}

// 光标绝对定位。行列都是 1 基（ANSI 约定），入参是 0 基网格坐标。
void appendCursorTo(std::string &out, uint32_t row, uint32_t col)
{
    // 左上角是每次整帧重绘的起点，单独走最短形式（"ESC[H" 比 "ESC[1;1H" 省 4 字节）。
    if (row == 0 && col == 0)
    {
        out += "\033[H";
        return;
    }

    out += "\033[";
    out += std::to_string(row + 1);
    out += ';';
    out += std::to_string(col + 1);
    out += 'H';
}

void appendTrueColor(std::string &out, const AsciiCell &c)
{
    out += "\033[38;2;";
    out += std::to_string(c.r);
    out += ';';
    out += std::to_string(c.g);
    out += ';';
    out += std::to_string(c.b);
    out += 'm';
}

// sRGB8 → xterm 色立方通道档位（0..5）。
// 色立方的代表值是等间距的 {0, 51, 102, 153, 204, 255}，所以就是 round(v / 51)。
// 用 (v*5 + 127) / 255 做整数四舍五入，避免浮点。
inline uint32_t cubeLevel(uint8_t v) noexcept
{
    return (static_cast<uint32_t>(v) * 5u + 127u) / 255u;
}

void append256Color(std::string &out, const AsciiCell &c)
{
    const uint32_t index = 16u + 36u * cubeLevel(c.r) + 6u * cubeLevel(c.g) + cubeLevel(c.b);
    out += "\033[38;5;";
    out += std::to_string(index);
    out += 'm';
}

void appendColor(std::string &out, const AsciiCell &c, bool trueColor)
{
    if (trueColor)
        appendTrueColor(out, c);
    else
        append256Color(out, c);
}

} // namespace

bool AsciiRenderer::resize(uint32_t width, uint32_t height)
{
    if (width == gridWidth_ && height == gridHeight_)
        return false;

    gridWidth_ = width;
    gridHeight_ = height;

    // 网格形状变了，上一帧缓存的行列对不上，必须整帧重发。
    // 返回 true 让调用方知道「这次真的要重建」，与 Framebuffer::resize 的约定一致。
    fullRedraw_ = true;
    return true;
}

bool AsciiRenderer::beginFrame(const render::FrameInfo &info)
{
    const auto start = std::chrono::steady_clock::now();

    stats_ = AsciiStats{};
    stats_.gridWidth = gridWidth_;
    stats_.gridHeight = gridHeight_;

    // 网格尺寸为 0 或没有视图 —— 明确失败，不去猜一个默认尺寸。
    // 返回 false 时 pixelBuffer_ / cells_ 保持上一帧内容，调用方不应读取。
    if (gridWidth_ == 0 || gridHeight_ == 0 || info.view == nullptr)
        return false;

    // 复制一份 View 再交给 drawScene。
    //
    // 这里有两套尺寸，单位不同，混淆一次就出画面级 bug，所以写清楚：
    //
    //   1) View::viewport / pixelBuffer_ 的单位是【像素】。
    //      超采样就发生在这里：光栅化在一个更高分辨率的缓冲上跑，
    //      再由 downsample() 平均回字符格。所以像素尺寸 = 字符格 × supersample。
    //
    //   2) camera.aspect 描述的是【字符格】的宽高比，不是像素视口的宽高比。
    //      终端字符单元通常只有其高度的一半宽，这个 cellAspectRatio 是
    //      **终端字体的属性**，不是场景的属性。让每个调用方自己记得乘，
    //      漏一次画面就被横向拉扁，而症状看起来像「透视投影写错了」，极难定位。
    //      放在这里就只有一处定义。
    //
    // 特别注意：camera.aspect 绝不能用 viewport.aspect() 顶替 —— 像素视口
    // 比例恰是 gridWidth_/gridHeight_，会整个丢掉 cellAspectRatio 校正，
    // 画面横向被拉伸 1/cellAspectRatio 倍。
    //
    // 复制成本：Camera + Viewport + RenderSettings，几十字节，每帧一次可忽略。
    const uint32_t supersample = std::max(1u, settings_.supersample);

    render::View view = *info.view;
    view.viewport.width = gridWidth_ * supersample;
    view.viewport.height = gridHeight_ * supersample;
    view.camera.aspect =
        (static_cast<float>(gridWidth_) * settings_.cellAspectRatio) / static_cast<float>(gridHeight_);

    render::FrameInfo frame = info;
    frame.view = &view;

    // 这一段和 CpuRenderer::beginFrame 是同一句话 —— 同一个 drawScene，同一套
    // 剔除 / 排序 / 光栅化 / 着色。区别只在下游怎么落地。
    // drawScene 会把 pixelBuffer_ resize 到 viewport 尺寸（= 字符格 × supersample），
    // 紧随其后的 downsample() 再把它平均回 gridWidth_ × gridHeight_。
    const bool ok = cpu::drawScene(frame, *this, pixelBuffer_, rasterStats_, sceneStats_, drawItems_);
    if (!ok)
        return false;

    downsample(supersample);
    buildCells();

    const auto end = std::chrono::steady_clock::now();
    lastFrameSeconds_ = std::chrono::duration<double>(end - start).count();
    return true;
}

void AsciiRenderer::endFrame()
{
    // 与 CpuRenderer 一样：软件后端的「提交」已经在 beginFrame 里同步完成了。
    // 这里只把 cells_ 差分成要发往终端的字节，不做任何 IO —— 写 fd 是平台层的事。
    buildOutput();
}

void downsampleLinear(const cpu::Framebuffer &src, uint32_t supersample, cpu::Framebuffer &dst)
{
    if (supersample == 0)
        supersample = 1;

    // 防御：调用方承诺 src 是 dst 的 supersample 倍。真出现尺寸不足，
    // 说明有人绕过 beginFrame 直接动了内部状态；输出黑帧总比越界读好。
    if (src.width() < dst.width() * supersample || src.height() < dst.height() * supersample)
    {
        dst.clear(math::Vec3{});
        return;
    }

    if (supersample == 1)
    {
        // 1:1 时直接搬，跳过一次多余的乘除。
        for (uint32_t y = 0; y < dst.height(); ++y)
            for (uint32_t x = 0; x < dst.width(); ++x)
                dst.setColor(x, y, src.color(x, y));
        return;
    }

    const float inverse = 1.0f / static_cast<float>(supersample * supersample);
    for (uint32_t y = 0; y < dst.height(); ++y)
    {
        for (uint32_t x = 0; x < dst.width(); ++x)
        {
            math::Vec3 sum{};
            const uint32_t x0 = x * supersample;
            const uint32_t y0 = y * supersample;
            for (uint32_t sy = 0; sy < supersample; ++sy)
                for (uint32_t sx = 0; sx < supersample; ++sx)
                    sum += src.color(x0 + sx, y0 + sy);

            dst.setColor(x, y, sum * inverse);
        }
    }
}

void AsciiRenderer::downsample(uint32_t supersample)
{
    gridBuffer_.resize(gridWidth_, gridHeight_);
    downsampleLinear(pixelBuffer_, supersample, gridBuffer_);
}

void AsciiRenderer::buildCells()
{
    const size_t count = static_cast<size_t>(gridWidth_) * gridHeight_;
    cells_.resize(count);

    // 亮度图从**降采样后**的缓冲抽取，和下面的取色同源。
    // 若亮度取自超采样缓冲、颜色取自降采样缓冲，轮廓处会出现「亮度说这里
    // 很暗、颜色却是亮的」的错位，边缘增强会挑出错误的格子。
    gridBuffer_.extractLuminance(luminance_);

    stats_.glyphs = 0;
    stats_.edgeCells = 0;
    float lowest = 1.0f;
    float highest = 0.0f;

    for (uint32_t y = 0; y < gridHeight_; ++y)
    {
        for (uint32_t x = 0; x < gridWidth_; ++x)
        {
            const size_t index = static_cast<size_t>(y) * gridWidth_ + x;

            // selectGlyph 一次给出：亮度量化（含 Bayer 抖动）+ Sobel 边缘方向字符。
            const Glyph glyph = selectGlyph(settings_.charset, luminance_.data(), gridWidth_,
                                            gridHeight_, x, y, settings_.glyph);

            AsciiCell &cell = cells_[index];
            cell.codePoint = glyph.codePoint;
            cell.level = glyph.level;
            cell.isEdge = glyph.isEdge;

            const math::Vec3 &rgb = gridBuffer_.color(x, y);
            cell.r = cpu::Framebuffer::encodeSrgb8(rgb.x);
            cell.g = cpu::Framebuffer::encodeSrgb8(rgb.y);
            cell.b = cpu::Framebuffer::encodeSrgb8(rgb.z);

            if (glyph.codePoint != U' ')
                ++stats_.glyphs;
            if (glyph.isEdge)
                ++stats_.edgeCells;

            const float l = luminance_[index];
            lowest = std::min(lowest, l);
            highest = std::max(highest, l);
        }
    }

    if (count == 0)
    {
        lowest = 0.0f;
        highest = 0.0f;
    }
    stats_.luminanceMin = lowest;
    stats_.luminanceMax = highest;
}

void AsciiRenderer::buildOutput()
{
    output_.clear();
    stats_.cellsChanged = 0;

    const size_t count = cells_.size();
    if (count == 0)
    {
        stats_.bytes = 0;
        return;
    }

    // 首帧，或网格尺寸变过 → 上一帧缓存的行列对不上，整帧重发。
    // 哨兵 codePoint 用 0：真实格最暗也是空格 U+0020，永远不相等。
    const bool full = fullRedraw_ || prevCells_.size() != count;
    if (full)
    {
        AsciiCell sentinel;
        sentinel.codePoint = 0;
        prevCells_.assign(count, sentinel);
    }

    const bool trueColor = settings_.trueColor;

    for (uint32_t y = 0; y < gridHeight_; ++y)
    {
        const size_t rowBase = static_cast<size_t>(y) * gridWidth_;
        uint32_t x = 0;

        while (x < gridWidth_)
        {
            if (sameCell(cells_[rowBase + x], prevCells_[rowBase + x]))
            {
                ++x;
                continue;
            }

            // 一段连续变化的格子共享一次光标定位 —— 定位序列本身要 5~10 字节，
            // 逐格定位会让它超过字形本身的体积。
            uint32_t end = x;
            while (end < gridWidth_ && !sameCell(cells_[rowBase + end], prevCells_[rowBase + end]))
                ++end;

            appendCursorTo(output_, y, x);
            appendColor(output_, cells_[rowBase + x], trueColor);

            for (uint32_t cx = x; cx < end; ++cx)
            {
                const AsciiCell &cell = cells_[rowBase + cx];
                // 同一段里颜色没变就不重发 SGR：一块颜色相同的墙面只发一次。
                if (cx != x && !sameColor(cell, cells_[rowBase + cx - 1]))
                    appendColor(output_, cell, trueColor);
                appendUtf8(output_, cell.codePoint);
            }

            stats_.cellsChanged += end - x;
            x = end;
        }
    }

    if (full)
        fullRedraw_ = false;

    prevCells_ = cells_;
    stats_.bytes = output_.size();
}

const char *AsciiRenderer::name() const noexcept { return "ascii"; }

void AsciiRenderer::readbackText(std::string &out) const
{
    out.clear();
    for (uint32_t y = 0; y < gridHeight_; ++y)
    {
        const size_t rowBase = static_cast<size_t>(y) * gridWidth_;
        for (uint32_t x = 0; x < gridWidth_; ++x)
            appendUtf8(out, cells_[rowBase + x].codePoint);
        out += '\n';
    }
}

bool AsciiRenderer::readbackImage(asset::ImageData &out)
{
    if (gridWidth_ == 0 || gridHeight_ == 0 || cells_.empty())
        return false;

    // 把字符格的前景色烘成一张 w x h 的 RGBA8 图。
    //
    // 注意这是**颜色**不是**字形** —— 一个格子在图像里只体现为一个像素，
    // 拿它和像素后端做 golden 对比时要知道这个差异。字形用 readbackText 取。
    out.width = gridWidth_;
    out.height = gridHeight_;
    out.colorSpace = asset::ColorSpace::SRGB;
    out.pixels.assign(cells_.size() * 4, 255);

    for (size_t i = 0; i < cells_.size(); ++i)
    {
        out.pixels[i * 4 + 0] = cells_[i].r;
        out.pixels[i * 4 + 1] = cells_[i].g;
        out.pixels[i * 4 + 2] = cells_[i].b;
    }

    return out.valid();
}

} // namespace my3d::backend::ascii
