#pragma once

// my3d::backend::cpu —— 软件路径上的着色求值。
//
// 这一层的唯一职责是把「场景光照」变成「某个表面点上的一次求值」。
// 它**不重新实现光照数学**：Lambert、距离衰减、球谐卷积全部转发给
// render::toolbox。三种后端共用同一份光照定义，画面之间才有可比性；
// 一旦这里出现第二份实现，两边必然漂移，而且会以「同一个模型在不同后端
// 亮度不一样」这种极难定位的形式暴露出来。

#include "core/math.hpp"
#include "scene/world.hpp"

#include <vector>

namespace my3d::backend::cpu
{

// 一帧光照参数的快照。每帧构造一次，而不是每个片段回查 World。
struct Lighting
{
    // 从 World 抽取光源、球谐环境光与常量环境项。空 World 也能安全调用。
    static Lighting from(const scene::World &world);

    std::vector<scene::LightDesc> lights;
    math::Vec3 sh[9]{};
    bool hasSH = false; // shBands 全零表示未设置，此时才回退到 ambientColor
    math::Vec3 ambient{0.02f, 0.02f, 0.02f};
};

// Lambert 直接光 + 球谐环境光。
// normal 必须是**单位向量** —— 光栅器已经在插值后重新归一化；
// 传进未归一化的法线会让亮度整体偏亮，且偏差随三角形尺度变化。
math::Vec3 shade(const Lighting &lighting, const math::Vec3 &surfacePoint,
                 const math::Vec3 &normal, const math::Vec3 &albedo);

// 曝光 + Reinhard tonemap，把 HDR 线性值压进 [0,1]。
//
// 顺序必须「先曝光后 tonemap」：反过来的话高光被压缩之后又按曝光拉亮，
// 结果是一片永远过曝的白。Reinhard 而非常数截断，是为了让高光有滚降 ——
// clamp 会在阈值处硬切成平板，反而比过曝更假。
math::Vec3 tonemap(const math::Vec3 &linear, float exposure) noexcept;

} // namespace my3d::backend::cpu
