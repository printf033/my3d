#pragma once

#include "material.hpp"
#include <vector>
#include <memory>
#include <cstdint>
#include <math/mat4.h>
#include <filament/Box.h>

struct Mesh
{
    Material material;
    filament::Box bound;
    uint32_t indexOffset = 0U;
    uint32_t indexCount = 0U;
};
