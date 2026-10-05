#pragma once

// my3d::scene —— 光照描述（后端无关）。

#include "core/math.hpp"

#include <cstdint>
#include <string>

namespace my3d::scene
{

enum class LightType : uint8_t
{
    Directional,
    Point
};

// 一个光源。字段刻意与 Filament 的 DirectionalLight / PointLight 同构，
// 但不 include 任何 Filament 头 —— 后端迁移时逐字段映射即可。
struct LightDesc
{
    std::string name;
    LightType type = LightType::Directional;
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;

    // Directional：光的传播方向（从光源射向场景），使用前会归一化。
    math::Vec3 direction{0.0f, -1.0f, 0.0f};

    // Point
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    float range = 0.0f;   // <= 0 表示无限远
    bool castShadows = false;

    // 从表面点指向光源的单位向量。CPU 光栅器与调试工具共用。
    math::Vec3 toLight(const math::Vec3 &surfacePoint) const noexcept
    {
        if (type == LightType::Directional)
            return -math::normalize(direction);

        const math::Vec3 d = position - surfacePoint;
        const float len = math::length(d);
        return len > 1e-6f ? d / len : math::Vec3{0.0f, 1.0f, 0.0f};
    }

    // 该光源在给定距离上的强度衰减：方向光恒为 1；
    // 点光用平方反比，并在 range 边界内做平滑收尾。
    float attenuation(float distanceToLight) const noexcept
    {
        if (type == LightType::Directional)
            return 1.0f;

        float a = 1.0f / (1.0f + distanceToLight * distanceToLight);
        if (range > 0.0f)
        {
            const float t = math::clamp(1.0f - distanceToLight / range, 0.0f, 1.0f);
            a *= t * t;
        }
        return a;
    }
};

} // namespace my3d::scene
