#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <math/vec4.h>
#include <filament/Material.h>

struct Material
{
    filament::MaterialInstance *material = nullptr;
    filament::math::float4 albedo{1.0f, 1.0f, 1.0f, 1.0f};
    float normal = 1.0f;
    // float occlusion = 1.0f;
    float roughness = 1.0f;
    float metallic = 1.0f;
    // ...
};