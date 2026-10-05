// my3d::backend::ascii —— 字符映射实现。依赖边界与数值约定的说明见 charset.hpp 顶部。

#include "backend/ascii/charset.hpp"

#include "core/math.hpp"

#include <cmath>

namespace my3d::backend::ascii
{
namespace
{

// 内部 UTF-8 解码：makeDefault/makeBlocks 的 ramp 字面量是唯一真源，glyphs 由它解出来。
// 为什么不直接手写两个列表（ramp + glyphs）：两份手工维护的等长列表迟早漂移，
// 典型症状是「某个字形永远选不到」，而这种 bug 在画面里只表现为「某档亮度怪怪的」。
//
// 容错策略：坏首字节跳一个字节继续、尾部截断直接丢。ramp 是我们自己的字面量，
// 出现坏字节只可能是有人手改错了；此时宁可少一个字形，也不要死循环或抛异常。
// 也正因为如此，这里不返回值、不报错。
void decodeUtf8(const std::string &text, std::vector<char32_t> &out)
{
    const size_t n = text.size();
    size_t i = 0;
    while (i < n)
    {
        const uint8_t b0 = static_cast<uint8_t>(text[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (b0 < 0x80u)
        {
            cp = b0;
            len = 1;
        }
        else if ((b0 & 0xE0u) == 0xC0u)
        {
            cp = b0 & 0x1Fu;
            len = 2;
        }
        else if ((b0 & 0xF0u) == 0xE0u)
        {
            cp = b0 & 0x0Fu;
            len = 3;
        }
        else if ((b0 & 0xF8u) == 0xF0u)
        {
            cp = b0 & 0x07u;
            len = 4;
        }
        else
        {
            ++i;   // 非法的首字节（含 continuation 字节开头）：跳过去
            continue;
        }

        if (i + len > n)
            break;   // 多字节序列被截断：丢掉尾巴

        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            const uint8_t bk = static_cast<uint8_t>(text[i + k]);
            if ((bk & 0xC0u) != 0x80u)
            {
                ok = false;
                break;
            }
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        if (!ok)
        {
            ++i;
            continue;
        }

        out.push_back(static_cast<char32_t>(cp));
        i += len;
    }
}

// 在覆盖率表里找与目标墨迹量 ink 最接近的档位（不做抖动时使用）。
//
// 线性扫描而不是二分：档位数是个位数到几十（默认表 10 档），线性扫描分支可预测、
// 更快也更好读；将来真上 256 级灰度表再换二分。
// 这里只求「最近」，不假设单调性 —— 换取表时的小出入不会选得很离谱。
// 前置条件：coverage 非空（调用方在 hasCoverage 分支里保证）。
uint8_t nearestCoverage(const std::vector<float> &coverage, float ink) noexcept
{
    size_t best = 0;
    float bestDist = std::fabs(coverage[0] - ink);
    for (size_t i = 1; i < coverage.size(); ++i)
    {
        const float d = std::fabs(coverage[i] - ink);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return static_cast<uint8_t>(best);
}

// 覆盖率空间里的有序抖动：先定位 ink 落在哪两档之间，再按「ink 在区间内的比例」
// 用 Bayer 阈值决定这一格是否上取一档。
//
// 为什么不写成「ink 加噪声再取最近档」：
//   * 阈值形式在区间端点是**恒等**的。ink 恰好等于某档覆盖率时 frac = 0，不抖；
//     ink = 0（纯黑）永远停在第一档（空格），ink 超过最高档覆盖率（默认 ramp 的
//     '@' 只有 0.62）永远停在最亮字形。而「加噪声再夹紧」会把纯黑顶到第一档，
//     黑底整片浮起一层灰（默认 ramp 第一档 '.' 的覆盖率只有 0.03，仍然看得见），
//     那是系统性偏差，不是抖动。
//   * 均值精确保持：一组 16 个格点上取上一档的比例恰好是 frac，平均墨迹量等于 ink，
//     误差上界是 1/32 个抖动态，肉眼不可见。
//   * 阈值只跟 (x,y) 有关，空间上固定 —— 静止画面不闪。
uint8_t ditheredLevel(const std::vector<float> &coverage, float ink, uint32_t x, uint32_t y) noexcept
{
    size_t base = 0;
    while (base + 1 < coverage.size() && coverage[base + 1] <= ink)
        ++base;

    const size_t next = base + 1;
    if (next >= coverage.size())
        return static_cast<uint8_t>(base);   // 已经在最亮档，没得抖

    const float lo = coverage[base];
    const float span = coverage[next] - lo;
    if (!(span > 0.0f))
        return static_cast<uint8_t>(base);   // 相邻两档覆盖率相同（重复字形）：插不了值

    const float frac = (ink - lo) / span;
    return static_cast<uint8_t>(bayer4x4(x, y) < frac ? next : base);
}

} // namespace

// ---------------------------------------------------------------- Charset

Charset Charset::makeDefault()
{
    Charset cs;
    cs.ramp = " .:-=+*#%@";
    decodeUtf8(cs.ramp, cs.glyphs);

    // 手工估计的墨迹覆盖率（近似值，不是实测，见头文件里的说明）：
    //   ' ' 恒为 0；'.' 只有几个百分点；':' '-' '=' '+' '*' 单调上升；
    //   '#' '%' '@' 三者挤在 0.40~0.62 —— 这不是笔误，是字形本身的特点：
    //   笔画粗到一定程度后覆盖率增长很慢（'@' 只是把 '#" 的环压扁并外扩），
    //   而人眼感知亮度还受字形密度影响（终端字体下 '%' 的斜杠加两点
    //   看上去和 '#' 差不多黑）。按等间距索引，0.5 的亮度会挑到第 6 档 '+'
    //   （墨迹仅 0.19），整幅图偏暗；按覆盖率反查才会挑到 '%' 或 '@'。
    // 不变量：与 glyphs 等长、单调不减（nearestCoverage 不做单调性假设，
    // 但顺序错乱会让画面出现「随机字形」，所以调用方换表时必须保持单调）。
    cs.coverage = {0.00f, 0.03f, 0.08f, 0.11f, 0.15f, 0.19f, 0.25f, 0.40f, 0.48f, 0.62f};
    return cs;
}

Charset Charset::makeBlocks()
{
    Charset cs;
    cs.ramp = " ░▒▓█";
    decodeUtf8(cs.ramp, cs.glyphs);

    // 阴影字符的覆盖率是**定义出来的**，不是估计：Unicode 码表规定
    // U+2591 ░ 为 25%、U+2592 ▒ 为 50%、U+2593 ▓ 为 75% 的墨迹，
    // U+2588 █ 是实心（100%）。所以这张表比 ASCII ramp 可靠得多，
    // 缺点是只有 5 档；低亮度区（0 与 0.25 之间）没有字形可用，
    // 这时抖动就格外重要 —— 4x4 Bayer 能在这段区间里补出中间调。
    cs.coverage = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    return cs;
}

uint8_t Charset::levels() const noexcept
{
    // glyphs 与 coverage 必须等长（不变量），所以任取一个作为档位数。
    // 截断到 255：uint8_t 回绕会把 256 档变成 0 档（空字符集）这种静默故障，
    // 而 255 档以上的字符分辨率在一个终端 cell 里没有意义。
    const size_t n = glyphs.size();
    return n > 255u ? static_cast<uint8_t>(255u) : static_cast<uint8_t>(n);
}

// ---------------------------------------------------------------- 抖动

float bayer4x4(uint32_t x, uint32_t y) noexcept
{
    // 经典 4x4 Bayer 矩阵（元素为 0..15）。按定义它由递归加法生成，
    // 天然平铺：取 x,y 的低两位即可，与图像尺寸无关，也不需要预计算表。
    static constexpr uint8_t kMatrix[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };

    // +0.5 是取格子中心，值域变成 [0.5/16, 15.5/16) = [0.03125, 0.96875) ⊂ [0,1)，
    // 16 个值互不相同。不取中心的话阈值会落在 0 与 1 两个端点上，端点会让
    // 某一档「永远亮」或「永远暗」，等于白白丢掉一档。
    const float t = static_cast<float>(kMatrix[y & 3u][x & 3u]) + 0.5f;
    return t * (1.0f / 16.0f);
}

uint8_t quantize(float luminance01, uint32_t x, uint32_t y, uint8_t levels, bool dither) noexcept
{
    // 0 档 = 没有字形可画；1 档 = 无论亮度都只能是那一个字形。两者都直接给 0，
    // 免得下面的除零/减一变成 UB。
    if (levels <= 1u)
        return 0u;

    // NaN 必须先单独挡掉（只挡 NaN，不挡 ±Inf）：math::clamp 的比较对 NaN 恒为假，
    // clamp 会把 NaN 原样放行；而后续 float → uint32_t 的转换在值为 NaN 时超出
    // 可表示范围，是 UB（-fsanitize=undefined 的 float-cast-overflow 会直接报）。
    // NaN 只可能来自光栅化的 0/0（退化三角形）或未初始化的缓冲，按纯黑处理比让
    // UB 传播安全。±Inf 则交给 clamp：+Inf 是 HDR 爆亮，应该给最亮一档，而不是变黑。
    float v = luminance01;
    if (std::isnan(v))
        v = 0.0f;

    // HDR / 曝光超调会让亮度越出 [0,1]；后面所有数学都建立在 lum ∈ [0,1] 上。
    v = math::clamp(v, 0.0f, 1.0f);

    // v >= 0 保证截断即 floor；v == 1 时 scaled == levels，直接归到最亮档。
    const float scaled = v * static_cast<float>(levels);
    const uint32_t base = static_cast<uint32_t>(scaled);
    if (base >= levels)
        return static_cast<uint8_t>(levels - 1u);

    if (dither)
    {
        // 抖动用**阈值比较**，不是「加噪声再夹紧」：
        //   scaled - base = frac 就是 v 在这一档区间内的位置，让 16 个 Bayer 阈值中
        //   小于 frac 的格点上取高一档，于是上取比例恰为 frac，一组像素的平均亮度
        //   精确保持。而「加噪声再夹紧」在端点上是系统性偏差：纯黑会被顶上一档
        //   （黑底浮起一层灰），且截断后均值不再等于原亮度。
        //   阈值只跟 (x,y) 有关，所以静止画面不闪 —— 这才是选 Bayer 而非白噪声的原因。
        const float frac = scaled - static_cast<float>(base);
        if (frac > 0.0f && base + 1u < levels && bayer4x4(x, y) < frac)
            return static_cast<uint8_t>(base + 1u);
    }
    return static_cast<uint8_t>(base);
}

// ---------------------------------------------------------------- UTF-8

size_t encodeUtf8(char32_t codePoint, char out[4]) noexcept
{
    // 契约外输入的唯一护栏：out 为 null 时直接放弃，而不是写崩。
    if (out == nullptr)
        return 0;

    // 非法码点的行为是**定义好的**，不是「未定义」：
    //   * > U+10FFFF（例如 0x110000）超出 Unicode 码位空间；
    //   * U+D800..U+DFFF 是 UTF-16 代理区，UTF-8 里不允许出现
    //     （标准称之为 surrogate，编码它会产生无法解码的「CESU-8」）。
    // 两者都替换成 U+FFFD REPLACEMENT CHARACTER（EF BF BD）并返回 3。
    // 理由：返回 0 或写 4 个垃圾字节会让调用方在拼接整行输出时错位，
    // 而且调用方通常懒得检查返回值 —— 保证「写出的永远是合法 UTF-8」
    // 比保证「如实反映输入」更重要。
    if (codePoint > 0x10FFFFu || (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
        codePoint = 0xFFFDu;

    if (codePoint <= 0x7Fu)
    {
        out[0] = static_cast<char>(codePoint);
        return 1;
    }
    if (codePoint <= 0x7FFu)
    {
        out[0] = static_cast<char>(0xC0u | (codePoint >> 6));
        out[1] = static_cast<char>(0x80u | (codePoint & 0x3Fu));
        return 2;
    }
    if (codePoint <= 0xFFFFu)
    {
        out[0] = static_cast<char>(0xE0u | (codePoint >> 12));
        out[1] = static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (codePoint & 0x3Fu));
        return 3;
    }
    // U+10000..U+10FFFF：4 字节，其余码位已在上面被 U+FFFD 顶掉，
    // 所以这里不需要再处理 5/6 字节的旧式扩展。
    out[0] = static_cast<char>(0xF0u | (codePoint >> 18));
    out[1] = static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
    out[3] = static_cast<char>(0x80u | (codePoint & 0x3Fu));
    return 4;
}

// ---------------------------------------------------------------- 边缘

EdgeSample sobel(const float *luminance, uint32_t w, uint32_t h, uint32_t x, uint32_t y) noexcept
{
    EdgeSample out;
    if (luminance == nullptr || w == 0u || h == 0u)
        return out;   // 退化输入：当作平坦区域，调用方会退回亮度字形

    // 坐标越界同样夹到边界，而不是让 r1[x] 读到下一行或数组外：
    // noexcept 接口没法报错，内存安全优先于「帮调用方 debug」。
    if (x >= w)
        x = w - 1u;
    if (y >= h)
        y = h - 1u;

    // 边界像素取不到完整 3x3 邻域。这里 clamp 到边缘像素（repeat 采样）：
    //   * 直接 return 0 会在画面四周留一圈「永远没有边缘」的边框，而 3D 物体的
    //     轮廓经常正好压在画幅边上，那圈空白非常显眼；
    //   * 直接读邻域则是越界读（UB），ASan 一跑就炸。
    // repeat 采样的代价是把边缘像素重复计入，梯度略偏小，但方向不会错。
    const uint32_t xm = (x == 0u) ? 0u : (x - 1u);
    const uint32_t xp = (x + 1u >= w) ? (w - 1u) : (x + 1u);
    const uint32_t ym = (y == 0u) ? 0u : (y - 1u);
    const uint32_t yp = (y + 1u >= h) ? (h - 1u) : (y + 1u);

    const float *r0 = luminance + static_cast<size_t>(ym) * w;
    const float *r1 = luminance + static_cast<size_t>(y) * w;
    const float *r2 = luminance + static_cast<size_t>(yp) * w;

    const float tl = r0[xm], tc = r0[x], tr = r0[xp];
    const float ml = r1[xm],             mr = r1[xp];
    const float bl = r2[xm], bc = r2[x], br = r2[xp];

    // 标准 Sobel 核。符号约定：行号 y 向下增大（图像行优先），所以
    // Gx > 0 表示「右边更亮」、Gy > 0 表示「下边更亮」。
    const float gx = (tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl);
    const float gy = (bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr);

    // 归一化：对归一化浮点输入，一个完美的 0→1 阶跃（黑白边）在单轴上给出
    // 恰好 4.0 —— 权重 (1,2,1) 之和 4 乘以落差 1.0。所以除以 4 之后，
    // magnitude 的单位是「满量程阶跃」，1.0 = 整幅图最剧烈的对比。
    //   * edgeThreshold = 0.35 因此读作「局部对比度达到全量程的 35% 就算轮廓」，
    //     与分辨率、与帧缓冲位深无关（8 位输入先除以 255 归一化即可，
    //     0.35 对应 89/255，语义完全一样）。这正是必须归一化的理由：
    //     不归一化的话阈值会隐含依赖输入的量纲，换个后端就得重调。
    //   * 对角阶跃的理论极值是 sqrt(2)（gx、gy 同时接近极值），这里夹到 1.0
    //     以维持 [0,1] 的契约 —— 阈值比较不需要 >1 的分辨率。
    const float mag = std::sqrt(gx * gx + gy * gy) * 0.25f;
    // 若邻域里混进 NaN，gx 或 gy 会变成 NaN，clamp 对 NaN 无效 —— 这时直接按
    // 「无边缘」处理，保证 magnitude 始终落在 [0,1] 契约内（调用方会把它当平坦
    // 区域，退回亮度字形，而不是拿一个 NaN 去比较阈值）。±Inf 仍然走 clamp
    // （夹到 1.0，即最强边缘）。
    out.magnitude = std::isnan(mag) ? 0.0f : math::clamp(mag, 0.0f, 1.0f);

    // 平坦区域（gx == gy == 0）时 atan2(0,0) 定义为 0，此时角度**没有意义**：
    // 调用方必须先看 magnitude，不要直接把角度喂给 edgeGlyph。
    out.angleRadians = std::atan2(gy, gx);
    return out;
}

char32_t edgeGlyph(float angleRadians) noexcept
{
    constexpr float kPiF = 3.14159265358979f;

    // 关键的一步：Sobel 给的是**梯度**方向（亮度增长最快的方向），而字符要沿
    // **边缘走向**摆放，两者相差 90°。搞反的后果很具象：竖直的物体轮廓被画成
    // '-'，斜边全部反向，画面看起来「边缘在跟人反着走」。
    // 所以先把角度旋转 +90° 得到边缘走向。
    float edge = std::fmod(angleRadians + kPiF * 0.5f, kPiF);

    // 边缘走向是无向的（差 180° 是同一条直线），归一到 [0, π)。
    if (edge < 0.0f)
        edge += kPiF;

    // 4 个方向字符把 [0, π) 四等分，每个字形覆盖 ±22.5°，按角度递增排列：
    //   0      -> '-'  水平线
    //   π/4    -> '\'  左上-右下（图像行号向下增大，所以 (cos45, sin45) 是右下）
    //   π/2    -> '|'  竖直线
    //   3π/4   -> '/'  左下-右上
    constexpr char32_t kGlyphs[4] = {U'-', U'\\', U'|', U'/'};

    // +π/8 是「就近取整」：把每个 45° 扇区的中心对齐到对应表项。
    // 最后一段（角度接近 π，其实等价于 0）算出 4，用 & 3 绕回 '-'
    // —— 比 if 判断更短，也天然覆盖 0/π 的接缝。
    const int quadrant = static_cast<int>((edge + kPiF * 0.125f) / (kPiF * 0.25f));
    return kGlyphs[quadrant & 3];
}

// ---------------------------------------------------------------- 选择

Glyph selectGlyph(const Charset &charset, const float *luminance, uint32_t w, uint32_t h,
                  uint32_t x, uint32_t y, const GlyphOptions &options) noexcept
{
    Glyph g;   // 默认值就是空格：任何退化输入都安全地退化成「什么都不画」
    if (luminance == nullptr || w == 0u || h == 0u || x >= w || y >= h)
        return g;

    const uint8_t levels = charset.levels();
    if (levels == 0u || charset.glyphs.empty())
        return g;   // 空字符集：与其取 glyphs[0] 越界，不如什么都不画

    // 同上：NaN 不吃 clamp，这里显式退化成黑，免得 ink 变 NaN 后 nearestCoverage
    // 的所有比较都失败（会静默选出第 0 档，看起来像「随机变暗」）。
    float lum = luminance[static_cast<size_t>(y) * w + x];
    if (std::isnan(lum))
        lum = 0.0f;
    lum = math::clamp(lum, 0.0f, 1.0f);

    // coverage 与 glyphs 长度不一致，说明调用方换表时漏填了 —— 这是配置错误，
    // 但纯函数不能抛异常。退化成等间距量化：画面难看，但不会读到越界。
    // 长度超过 255 时同样退化：层级用 uint8_t 表达，256 档以上会被截断成错档，
    // 静默选错字形比退化成等间距难查得多（现实中不会出现，但宁可封住）。
    const size_t glyphCount = charset.glyphs.size();
    const bool hasCoverage = charset.coverage.size() == glyphCount && glyphCount <= 255u;

    uint8_t level = 0;
    if (hasCoverage)
    {
        // 按覆盖率反查：挑墨迹量最接近 ink 的字形。这是「等间距索引会让亮部
        // 丢层次」的正解（见 Charset 的注释）。表只有个位数到几十档，
        // 逐像素线性扫描的代价可以忽略（160x50 = 8000 像素 × 10 档）。
        //
        // 抖动在覆盖率空间里做，而不是先在亮度上加噪声：这样「纯黑仍是空格、
        // 纯白仍是最亮字形」是结构性保证，不依赖任何夹紧（见 ditheredLevel）。
        level = options.dither ? ditheredLevel(charset.coverage, lum, x, y)
                               : nearestCoverage(charset.coverage, lum);
    }
    else
    {
        // 没有可用的覆盖率表（或长度不匹配）：退回等间距量化。
        level = quantize(lum, x, y, levels, options.dither);
    }

    if (options.edges)
    {
        const EdgeSample sample = sobel(luminance, w, h, x, y);
        const float threshold = math::clamp(options.edgeThreshold, 0.0f, 1.0f);
        // 附带条件 magnitude > 0 是防「threshold 传 0」的护栏：平坦区域的梯度
        // 恰好是 0，若写成 >= 会把整幅图判成边缘，得到满屏方向字符。
        if (sample.magnitude > 0.0f && sample.magnitude >= threshold)
        {
            // 边缘像素也保留亮度层级：渲染器要用 level 取颜色/亮度，
            // 用 codePoint 取字形；而方向字符不在字符集里，没有对应层级。
            g.codePoint = edgeGlyph(sample.angleRadians);
            g.level = level;
            g.isEdge = true;
            return g;
        }
    }

    // level 一定落在 [0, glyphs.size()) 内：hasCoverage 分支下两个辅助函数返回的
    // 索引都 < coverage.size() == glyphCount ≤ 255；否则走 quantize，其上界是
    // levels() - 1 = min(glyphCount, 255) - 1。两条路径都不会越界。
    g.codePoint = charset.glyphs[level];
    g.level = level;
    g.isEdge = false;
    return g;
}

} // namespace my3d::backend::ascii
