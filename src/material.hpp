#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <math/vec4.h>
#include <filament/Material.h>

struct Material
{
    filament::MaterialInstance *material = nullptr;
    filament::math::float4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    filament::math::float4 emissiveFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float roughnessFactor = 1.0f;
    float metallicFactor = 1.0f;
    // ...
};