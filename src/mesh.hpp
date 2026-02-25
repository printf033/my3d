#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <math/mat4.h>
#include <filament/Box.h>
#include <filament/Material.h>

struct Mesh
{
    filament::Box bound;
    filament::MaterialInstance *material = nullptr;
    uint32_t indexOffset = 0U;
    uint32_t indexCount = 0U;
};
