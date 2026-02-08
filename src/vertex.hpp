#pragma once

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/quat.h>

struct Vertex
{
    filament::math::float3 xyz;
    filament::math::quatf tbn;
    filament::math::float2 uv;
};