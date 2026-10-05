#pragma once

// my3d::render —— 后端可选的复用工具箱。
//
// 「复用同一套代码」的真正落点在这里，而不在接口里：
//   - Filament 后端跳过这些（它有自己的剔除 / 排序 / 光照）；
//   - CPU / ASCII / 未来 Vulkan 后端使用它们。
//
// 不进接口的理由同 DrawItem：放进接口就会逼迫 Filament 绕过接口去自己重建，
// 抽象随即名存实亡。

#include "draw_item.hpp"
#include "scene/light.hpp"
#include "scene/world.hpp"

#include <vector>

namespace my3d::render
{

// 遍历场景，为每个可见节点的每个图元产出一个 DrawItem。
void buildDrawItems(const scene::World &world, std::vector<DrawItem> &out);

// 就地视锥剔除。
// 包围盒无效（valid == false）的条目**保留**：缺包围盒信息时保守地画出来，
// 比静默丢几何安全 —— 剔除应当只丢「确定在外」的东西。
void cullByFrustum(const math::Frustum &frustum, std::vector<DrawItem> &items);

// 就地排序。cameraPosition 仅在 FrontToBack / BackToFront 下使用。
void sortDrawItems(std::vector<DrawItem> &items, SortMode mode,
                   const math::Vec3 &cameraPosition = math::Vec3{0.0f, 0.0f, 0.0f});

// ---------------- 光照求值（CPU 路径共用） ----------------

// 单个光源在某表面点上的漫反射贡献（Lambert）。N 必须已归一化。
math::Vec3 evalDiffuse(const scene::LightDesc &light, const math::Vec3 &N,
                       const math::Vec3 &albedo, const math::Vec3 &surfacePoint);

// 方向光便利版本。
math::Vec3 evalDirectional(const scene::LightDesc &light, const math::Vec3 &N,
                           const math::Vec3 &albedo);

// 三阶球谐的环境辐照度。normal 必须已归一化。
math::Vec3 evalIrradianceSH(const math::Vec3 sh[9], const math::Vec3 &normal);

} // namespace my3d::render
