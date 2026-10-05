#pragma once

// 材质的引擎侧表示。
//
// 字段与旧代码已经使用的 assimp/glTF PBR 语义一一对应，不引入新概念：
//   baseColorFactor  ← AI_MATKEY_COLOR_DIFFUSE
//   emissiveFactor   ← AI_MATKEY_COLOR_EMISSIVE（.w 是 AI_MATKEY_EMISSIVE_INTENSITY）
//   roughnessFactor  ← AI_MATKEY_ROUGHNESS_FACTOR
//   metallicFactor   ← AI_MATKEY_METALLIC_FACTOR
// 三套着色模型对应 assets/shader/ 里现有的三个 .mat：
//   unlit.mat / lit.mat / lit_aorm.mat

#include "core/math.hpp"
#include "image_data.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace my3d::asset
{

enum class ShadingModel : uint8_t
{
    Unlit,    // 无光照：直接输出 baseColor * 贴图
    Lit,      // PBR：baseColor / normal / roughness / metallic / emissive
    LitAORM   // 在 Lit 之上再叠加 AO 通道
};

// 一张贴图的数据 + 来源标识。
// source 用于后端按 key 去重（旧代码拿文件路径当 unordered_map 的 key），
// shared_ptr 让同一张图在多个材质间共享，不必重复解码。
struct TextureSlot
{
    ImagePtr image;
    std::string source;   // 文件路径，或 "embedded:<index>" 表示 glTF 内嵌纹理

    bool empty() const noexcept { return image == nullptr; }
};

struct MaterialDesc
{
    std::string name;
    ShadingModel model = ShadingModel::Lit;

    math::Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    math::Vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};
    float roughnessFactor = 1.0f;
    float metallicFactor = 0.0f;
    float normalScale = 1.0f;
    float aoStrength = 1.0f;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool alphaMask = false;

    TextureSlot baseColor;
    TextureSlot emissive;
    TextureSlot normal;
    TextureSlot roughness;
    TextureSlot metallic;
    TextureSlot ambientOcclusion;
};

} // namespace my3d::asset
