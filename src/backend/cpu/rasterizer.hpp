#pragma once

// my3d::backend::cpu —— 与输出目标无关的软件光栅器。
//
// 它只做一件事：把一组已变换到 clip space 的三角形，变成 Framebuffer 上的
// 线性 RGB 像素。它不知道自己在画终端字符还是 PNG，也不持有场景 ——
// 这是「同一套代码既出 GPU 画面也出字符画面」的落点。
//
// 不做的事（有意为之，都是为了不把第一期做成本地 Vulkan）：
//   - 没有通用 shader 抽象，着色只是一个 std::function
//   - 没有纹理采样、没有 MSAA、没有多重渲染目标
//   - 没有命令缓冲 / 管线状态对象 / 描述符集
// 真需要这些的时候，它们是 backend/vulkan 的内部细节，不是这里的接口。

#include "core/math.hpp"
#include "framebuffer.hpp"

#include <cstdint>
#include <functional>

namespace my3d::backend::cpu
{

// 光栅器的顶点：clip space 位置 + 需要在光栅阶段插值的属性。
//
// 位置保留未除 w 的 clip 形式，因为后续两件事都要用到它：
//   - 近平面裁剪在 clip space 做（z + w >= 0 即位于近平面之前）；
//   - 透视校正插值需要原始的 1/w。
// 属性存**世界空间**值（不是视图空间），因为着色要用世界坐标做光照求值；
// 在这个尺度上多带三分量的代价远低于来回换空间的出错概率。
struct RasterVertex
{
    math::Vec4 clip{0.0f, 0.0f, 0.0f, 1.0f};
    math::Vec3 worldPosition{0.0f, 0.0f, 0.0f};
    math::Vec3 worldNormal{0.0f, 1.0f, 0.0f};
    math::Vec2 uv{0.0f, 0.0f};
};

// 单个屏幕像素上的插值结果。
struct Fragment
{
    math::Vec3 worldPosition{0.0f, 0.0f, 0.0f};
    math::Vec3 worldNormal{0.0f, 1.0f, 0.0f}; // 已重新归一化，着色可假定单位长
    math::Vec2 uv{0.0f, 0.0f};
    float ndcZ = 1.0f; // ∈ [-1, 1]，越小越近
    float invW = 1.0f; // 1/w_clip，供需要做透视相关效果（雾、深度重建）的调用方
};

// 着色器返回**线性** RGB，或标记片元丢弃（如 alpha mask）。光栅器不做任何
// 色彩处理：曝光、tonemap、sRGB 编码都属于调用方或 shading 层。
struct FragmentOutput
{
    math::Vec3 color{0.0f, 0.0f, 0.0f};
    bool discard = false;
};

using FragmentShader = std::function<FragmentOutput(const Fragment &)>;

struct RasterSettings
{
    bool cullBackFaces = true;
    bool depthTest = true;
    bool depthWrite = true;
};

struct RasterStats
{
    uint32_t trianglesSubmitted = 0;
    uint32_t trianglesCulled = 0;   // 背面剔除
    uint32_t trianglesClipped = 0;  // 完全落在近平面之后
    uint32_t trianglesDrawn = 0;    // 实际走了像素循环的（含裁剪后拆出的）
    uint64_t fragmentsTested = 0;   // 落入包围盒且通过覆盖测试的像素
    uint64_t fragmentsShaded = 0;   // 通过深度测试、真正跑了着色器的像素
};

// 光栅化一个三角形。内部会先做近平面裁剪：
// 跨越近平面的三角形被拆成 1~2 个，而不是整个丢弃 —— 丢弃会让近处大面片
// 突然整块消失，留下一个洞，这比一条锯齿更难看。
//
// 视口与像素尺寸取自 framebuffer，缩放/平移在所有三角形间共享。
void rasterizeTriangle(Framebuffer &framebuffer, const RasterVertex &a, const RasterVertex &b,
                       const RasterVertex &c, const FragmentShader &shader,
                       const RasterSettings &settings, RasterStats &stats);

// 线段光栅，供线框模式使用。深度测试与三角形一致，不写深度（线框不该挡住自己）。
void rasterizeLine(Framebuffer &framebuffer, const RasterVertex &a, const RasterVertex &b,
                   const FragmentShader &shader, const RasterSettings &settings,
                   RasterStats &stats);

} // namespace my3d::backend::cpu
