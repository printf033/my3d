#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <math/vec4.h>
#include <filament/Material.h>

struct Material
{
    filament::MaterialInstance *material = nullptr;
    filament::math::float4 diffuseFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float normalFactor = 1.0f;
    // float occlusionFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float metallicFactor = 1.0f;
    // ...
};