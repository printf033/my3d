#include "backend/cpu/rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace my3d::backend::cpu
{
namespace
{

// 已经除过 w、映射到像素坐标的顶点。属性保持**原始值**不预先除以 w，
// 因为透视校正的正确做法是「属性与 1/w 一起乘」，而不是先把属性除掉。
struct ScreenVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f; // NDC z
    float invW = 1.0f;
    math::Vec3 worldPosition{0.0f, 0.0f, 0.0f};
    math::Vec3 worldNormal{0.0f, 1.0f, 0.0f};
    math::Vec2 uv{0.0f, 0.0f};
};

inline float edgeFunction(float ax, float ay, float bx, float by, float px, float py) noexcept
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

ScreenVertex toScreen(const RasterVertex &v, uint32_t width, uint32_t height) noexcept
{
    const float invW = 1.0f / v.clip.w;
    ScreenVertex s;
    // y 要翻转：NDC 的 +y 朝上，像素行号朝下。这次翻转同时**反转了绕序**，
    // 背面判定的符号因此跟着变 —— 见 drawTriangle 里的推导。
    s.x = (v.clip.x * invW * 0.5f + 0.5f) * static_cast<float>(width);
    s.y = (0.5f - v.clip.y * invW * 0.5f) * static_cast<float>(height);
    s.z = v.clip.z * invW;
    s.invW = invW;
    s.worldPosition = v.worldPosition;
    s.worldNormal = v.worldNormal;
    s.uv = v.uv;
    return s;
}

inline RasterVertex lerpVertex(const RasterVertex &a, const RasterVertex &b, float t) noexcept
{
    const auto mix = [t](float x, float y) { return x + (y - x) * t; };

    RasterVertex r;
    r.clip = math::Vec4{mix(a.clip.x, b.clip.x), mix(a.clip.y, b.clip.y),
                        mix(a.clip.z, b.clip.z), mix(a.clip.w, b.clip.w)};
    r.worldPosition = math::Vec3{mix(a.worldPosition.x, b.worldPosition.x),
                                 mix(a.worldPosition.y, b.worldPosition.y),
                                 mix(a.worldPosition.z, b.worldPosition.z)};
    r.worldNormal = math::Vec3{mix(a.worldNormal.x, b.worldNormal.x),
                               mix(a.worldNormal.y, b.worldNormal.y),
                               mix(a.worldNormal.z, b.worldNormal.z)};
    r.uv = math::Vec2{mix(a.uv.x, b.uv.x), mix(a.uv.y, b.uv.y)};
    return r;
}

// 近平面单平面裁剪。clip space 里「位于近平面之前」等价于 z + w >= 0
// （即 z_ndc >= -1），因为 w > 0 时该式两边同除 w 不变号。
//
// 为什么必须裁剪而不能整三角形丢弃：一个横跨相机平面的大面片（地面、墙）
// 丢掉之后会留下一整块洞，比边缘的一点锯齿难看得多。
//
// 结果写进 out，返回生成几个三角形（0/1/2），每个三角形占 3 个连续顶点。
uint32_t clipAgainstNearPlane(const RasterVertex in[3], RasterVertex out[6]) noexcept
{
    float d[3];
    int insideCount = 0;
    for (int i = 0; i < 3; ++i)
    {
        d[i] = in[i].clip.z + in[i].clip.w;
        insideCount += (d[i] >= 0.0f) ? 1 : 0;
    }

    if (insideCount == 3)
    {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return 1;
    }
    if (insideCount == 0)
        return 0;

    // 沿三条边行走，保留内侧顶点与穿越点。最多 4 个顶点。
    RasterVertex poly[4];
    int n = 0;
    for (int i = 0; i < 3; ++i)
    {
        const int j = (i + 1) % 3;
        const bool iInside = d[i] >= 0.0f;
        const bool jInside = d[j] >= 0.0f;

        if (iInside)
            poly[n++] = in[i];

        if (iInside != jInside)
        {
            // d 沿边是线性的，交点参数就是两端的线性比例。
            // 分母不可能为 0：符号不同的两个值不会相等。
            const float t = d[i] / (d[i] - d[j]);
            poly[n++] = lerpVertex(in[i], in[j], t);
        }
    }

    if (n < 3)
        return 0; // 退化到一条线或一个点，没有可填充面积

    if (n == 3)
    {
        out[0] = poly[0];
        out[1] = poly[1];
        out[2] = poly[2];
        return 1;
    }

    // 四边形按 (0,1,2) + (0,2,3) 拆分，保持原绕序，两个三角形法线朝向一致。
    out[0] = poly[0];
    out[1] = poly[1];
    out[2] = poly[2];
    out[3] = poly[0];
    out[4] = poly[2];
    out[5] = poly[3];
    return 2;
}

void drawTriangle(Framebuffer &framebuffer, const RasterVertex &ra, const RasterVertex &rb,
                  const RasterVertex &rc, const FragmentShader &shader,
                  const RasterSettings &settings, RasterStats &stats)
{
    const uint32_t width = framebuffer.width();
    const uint32_t height = framebuffer.height();
    if (width == 0 || height == 0)
        return;

    const ScreenVertex v0 = toScreen(ra, width, height);
    const ScreenVertex v1 = toScreen(rb, width, height);
    const ScreenVertex v2 = toScreen(rc, width, height);

    const float area =
        edgeFunction(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y) ;
    if (area == 0.0f)
        return; // 退化，没有面积可填

    // 背面判定。屏幕 y 轴向下，于是 NDC 中的逆时针（OpenGL 约定的正面）
    // 在屏幕空间得到的是**负**面积。所以面积 > 0 即背面。
    // 这个符号极易写反，症状是「物体只有从内部才看得见」。
    if (area > 0.0f && settings.cullBackFaces)
    {
        ++stats.trianglesCulled;
        return;
    }

    // 面积符号会随绕序变，但重心坐标是「子三角形面积 / 总面积」，
    // 内部时恒为正 —— 所以内部判定统一写 >= 0，不必先规范化绕序。
    const float invArea = 1.0f / area;

    const float bx0 = std::min({v0.x, v1.x, v2.x});
    const float bx1 = std::max({v0.x, v1.x, v2.x});
    const float by0 = std::min({v0.y, v1.y, v2.y});
    const float by1 = std::max({v0.y, v1.y, v2.y});

    // 包围盒同时是屏幕裁剪：巨大的近处三角形只在其可见部分上循环。
    const int minX = std::max(0, static_cast<int>(std::floor(bx0)));
    const int maxX =
        std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(bx1)));
    const int minY = std::max(0, static_cast<int>(std::floor(by0)));
    const int maxY =
        std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(by1)));
    if (minX > maxX || minY > maxY)
        return;

    ++stats.trianglesDrawn;

    for (int py = minY; py <= maxY; ++py)
    {
        const float sampleY = static_cast<float>(py) + 0.5f;
        for (int px = minX; px <= maxX; ++px)
        {
            const float sampleX = static_cast<float>(px) + 0.5f;

            const float b0 =
                edgeFunction(v1.x, v1.y, v2.x, v2.y, sampleX, sampleY) * invArea;
            if (b0 < 0.0f)
                continue;
            const float b1 =
                edgeFunction(v2.x, v2.y, v0.x, v0.y, sampleX, sampleY) * invArea;
            if (b1 < 0.0f)
                continue;
            const float b2 =
                edgeFunction(v0.x, v0.y, v1.x, v1.y, sampleX, sampleY) * invArea;
            if (b2 < 0.0f)
                continue;

            ++stats.fragmentsTested;

            // 深度：z_ndc 是 1/w 的仿射函数，所以在屏幕空间**线性**插值即可，
            // 不需要透视校正 —— 这正是选用 NDC 深度而非 view 深度的原因。
            const float ndcZ = b0 * v0.z + b1 * v1.z + b2 * v2.z;

            if (settings.depthTest && !framebuffer.depthTest(static_cast<uint32_t>(px),
                                                             static_cast<uint32_t>(py), ndcZ))
                continue;

            // 其余属性要做透视校正：屏幕空间的线性插值只有在除以 w 之后
            // 才对应世界空间的线性分布。漏掉这一步的症状是纹理/法线随距离拉伸。
            const float iw0 = b0 * v0.invW;
            const float iw1 = b1 * v1.invW;
            const float iw2 = b2 * v2.invW;
            const float sumInvW = iw0 + iw1 + iw2;
            if (sumInvW <= 0.0f)
                continue; // w <= 0 已被近平面裁剪排除，纯属数值兜底

            const float k0 = iw0 / sumInvW;
            const float k1 = iw1 / sumInvW;
            const float k2 = iw2 / sumInvW;

            Fragment fragment;
            fragment.worldPosition = v0.worldPosition * k0 + v1.worldPosition * k1 +
                                     v2.worldPosition * k2;
            fragment.uv = v0.uv * k0 + v1.uv * k1 + v2.uv * k2;
            fragment.ndcZ = ndcZ;
            fragment.invW = sumInvW; // 1/w 本身在屏幕空间线性，插值结果就是它

            const math::Vec3 blendedNormal =
                v0.worldNormal * k0 + v1.worldNormal * k1 + v2.worldNormal * k2;
            const float normalLenSq = math::lengthSq(blendedNormal);
            // 法线插值后长度不再为 1，必须重新归一化，否则光照强度会偏。
            // 退化法线（顶点法线互相抵消）给一个朝上的默认值，避免 NaN 污染整片像素。
            fragment.worldNormal =
                normalLenSq > 1e-12f ? blendedNormal * (1.0f / std::sqrt(normalLenSq))
                                     : math::Vec3{0.0f, 1.0f, 0.0f};

            const FragmentOutput output = shader(fragment);
            if (output.discard)
                continue;

            if (settings.depthWrite)
                framebuffer.setDepth(static_cast<uint32_t>(px), static_cast<uint32_t>(py), ndcZ);

            framebuffer.setColor(static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                                 output.color);
            ++stats.fragmentsShaded;
        }
    }
}

} // namespace

void rasterizeTriangle(Framebuffer &framebuffer, const RasterVertex &a, const RasterVertex &b,
                       const RasterVertex &c, const FragmentShader &shader,
                       const RasterSettings &settings, RasterStats &stats)
{
    ++stats.trianglesSubmitted;

    const RasterVertex in[3] = {a, b, c};
    RasterVertex clipped[6];
    const uint32_t produced = clipAgainstNearPlane(in, clipped);
    if (produced == 0)
    {
        ++stats.trianglesClipped;
        return;
    }

    for (uint32_t t = 0; t < produced; ++t)
        drawTriangle(framebuffer, clipped[t * 3], clipped[t * 3 + 1], clipped[t * 3 + 2],
                     shader, settings, stats);
}

void rasterizeLine(Framebuffer &framebuffer, const RasterVertex &a, const RasterVertex &b,
                   const FragmentShader &shader, const RasterSettings &settings,
                   RasterStats &stats)
{
    const uint32_t width = framebuffer.width();
    const uint32_t height = framebuffer.height();
    if (width == 0 || height == 0)
        return;

    // 线段不做近平面裁剪（线很细，拆分的收益不值复杂度），
    // 两端只要有一个落在相机之后就直接丢弃。
    if (a.clip.w <= 0.0f || b.clip.w <= 0.0f)
        return;

    const ScreenVertex s0 = toScreen(a, width, height);
    const ScreenVertex s1 = toScreen(b, width, height);

    const float dx = s1.x - s0.x;
    const float dy = s1.y - s0.y;

    // 用 DDA 而非 Bresenham：需要在步进中同时插值属性与 1/w，
    // DDA 的浮点参数形式比整数误差累积更直接，也不会有累积漂移。
    int steps = static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy))));
    if (steps < 1)
        steps = 1;

    for (int i = 0; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);

        const int px = static_cast<int>(std::floor(s0.x + dx * t));
        const int py = static_cast<int>(std::floor(s0.y + dy * t));
        if (!framebuffer.contains(px, py))
            continue;

        const float ndcZ = s0.z + (s1.z - s0.z) * t;

        if (settings.depthTest && !framebuffer.depthTest(static_cast<uint32_t>(px),
                                                         static_cast<uint32_t>(py), ndcZ))
            continue;

        const float iw0 = (1.0f - t) * s0.invW;
        const float iw1 = t * s1.invW;
        const float sumInvW = iw0 + iw1;
        if (sumInvW <= 0.0f)
            continue;

        const float k0 = iw0 / sumInvW;
        const float k1 = iw1 / sumInvW;

        Fragment fragment;
        fragment.worldPosition = s0.worldPosition * k0 + s1.worldPosition * k1;
        fragment.uv = s0.uv * k0 + s1.uv * k1;
        fragment.ndcZ = ndcZ;
        fragment.invW = sumInvW;

        const math::Vec3 blendedNormal = s0.worldNormal * k0 + s1.worldNormal * k1;
        const float normalLenSq = math::lengthSq(blendedNormal);
        fragment.worldNormal = normalLenSq > 1e-12f
                                   ? blendedNormal * (1.0f / std::sqrt(normalLenSq))
                                   : math::Vec3{0.0f, 1.0f, 0.0f};

        // 线框不写深度：写进去会让线互相遮挡，边缘断断续续，
        // 而我们画线框的目的恰恰是看完整的结构。
        const FragmentOutput output = shader(fragment);
        if (output.discard)
            continue;
        framebuffer.setColor(static_cast<uint32_t>(px), static_cast<uint32_t>(py), output.color);
        ++stats.fragmentsTested;
        ++stats.fragmentsShaded;
    }
}

} // namespace my3d::backend::cpu
