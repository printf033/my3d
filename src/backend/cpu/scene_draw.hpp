#pragma once

// my3d::backend::cpu —— 与输出形式无关的一帧绘制流程。
//
// 这个函数是「同一套代码」的物理载体：CpuRenderer（像素输出）与
// AsciiRenderer（字符输出）都调用它，区别只在目标 Framebuffer 的像素尺寸、
// 以及拿到线性 RGB 结果之后做什么（8 位 sRGB 编码 vs 转字符）。
//
// 它不做 present、不做后处理、不碰色彩编码 —— 光栅结果保持线性，
// 由调用方决定如何落到具体设备上。

#include "asset/material.hpp"
#include "asset/mesh_data.hpp"
#include "backend/cpu/framebuffer.hpp"
#include "backend/cpu/rasterizer.hpp"
#include "backend/cpu/shading.hpp"
#include "render/draw_item.hpp"
#include "render/types.hpp"

#include <vector>

namespace my3d::backend::cpu
{

// 绘制期间的资源查询接口。
//
// 为什么不直接给 IRenderDevice：IRenderDevice 只有 create/destroy，没有
// 「按句柄取回」。把 get 加进 IRenderDevice 会迫使 GPU 后端（Filament）实现
// 一条它本来不需要的查询路径 —— 它的资源在引擎内部，取回来也没有意义。
// 这个查询只有 CPU 路径用，所以它留在这里。
class ResourceView
{
public:
    virtual ~ResourceView() = default;

    virtual const asset::MeshData *mesh(MeshHandle handle) const noexcept = 0;
    virtual const asset::MaterialDesc *material(MaterialHandle handle) const noexcept = 0;
};

// 一帧的粗粒度统计，供测试断言与调试输出。
struct SceneDrawStats
{
    uint32_t drawItems = 0;        // 剔除前的候选数
    uint32_t drawItemsCulled = 0;  // 被视锥剔除掉的
    uint32_t drawCalls = 0;        // 实际提交了几何体的条目数
    uint32_t triangles = 0;        // 提交的三角形数
};

// 把 info 里的场景画进 target。
//
// scratch 由调用方持有，用于避免每帧重新分配 DrawItem 数组（ASCII 后端可能
// 每帧调用多次：一次算字符、一次算颜色）。函数内部会清空它再填充。
//
// 返回 false 表示输入不完整：world 或 view 为空，或视口尺寸为零。
// 此时 target 不被触碰。
bool drawScene(const render::FrameInfo &info, const ResourceView &resources,
               Framebuffer &target, RasterStats &stats, SceneDrawStats &sceneStats,
               std::vector<render::DrawItem> &scratch);

} // namespace my3d::backend::cpu
