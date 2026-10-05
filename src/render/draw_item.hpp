#pragma once

// my3d::render —— 剔除 / 排序后的中间表示。

#include "core/handle.hpp"
#include "core/math.hpp"

#include <cstdint>

namespace my3d::render
{

// DrawItem 是后端**内部**的中间产物，不是接口的主语。
//
//   - CPU 光栅器（含 ASCII）消费它；
//   - Filament 后端不用它 —— 它自带场景管理，绕过这条路径；
//   - 未来 Vulkan 后端消费它。
//
// 它的价值在于把「视锥剔除 + 排序」这份逻辑收敛到一处，而不是每个自研后端
// 各写一遍。把它塞进 IRenderer 接口是错的：那会逼迫 Filament 绕过接口。
struct DrawItem
{
    MeshHandle mesh;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    MaterialHandle material;
    math::Mat4 worldTransform = math::Mat4::identity();   // 列主序、16 float
    math::AABB worldBounds;
    uint32_t sortKey = 0;
};

enum class SortMode
{
    None,
    FrontToBack,
    BackToFront,
    ByMaterial
};

} // namespace my3d::render
