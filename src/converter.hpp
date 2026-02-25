#pragma once

#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>
#include <assimp/color4.h>
#include <math/mat4.h>
#include <math/vec4.h>
#include <math/vec3.h>
#include <math/quat.h>

struct Converter
{
    static inline filament::math::mat4f assimp2filament(aiMatrix4x4 &src) noexcept
    {
        return filament::math::mat4f(
            src.a1, src.b1, src.c1, src.d1,
            src.a2, src.b2, src.c2, src.d2,
            src.a3, src.b3, src.c3, src.d3,
            src.a4, src.b4, src.c4, src.d4);
    }
    static inline filament::math::float3 assimp2filament(aiVector3D &src) noexcept
    {
        return filament::math::float3(
            src.x, src.y, src.z);
    }
    static inline filament::math::float4 assimp2filament(aiColor4D &src) noexcept
    {
        return filament::math::float4(
            src.r, src.g, src.b, src.a);
    }
};