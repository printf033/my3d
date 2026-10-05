#pragma once

// my3d::backend::cpu —— 软件路径的资源容器。
//
// 它同时扮演两个角色：
//   * IRenderDevice —— 场景代码用它创建网格 / 纹理 / 材质；
//   * ResourceView  —— drawScene 用它按句柄取回资源。
//
// 两个角色合在一个类里，是为了让 CpuRenderer 与 AsciiRenderer **各自**持有
// 一份而共用同一套绘制流程。资源本来就该由各自的后端持有（GPU 后端的资源在
// Filament 引擎里，不可能被 CPU 光栅器直接读），共享的是流程而不是存储。

#include "asset/image_data.hpp"
#include "asset/material.hpp"
#include "asset/mesh_data.hpp"
#include "backend/cpu/scene_draw.hpp"
#include "core/handle.hpp"
#include "render/renderer.hpp"

namespace my3d::backend::cpu
{

// 纹理记录：像素 + 用途。
//
// 用途不能丢：BaseColor / Emissive 以 sRGB 存储，采样时要先解码到线性；
// Normal / MetallicRoughness / AmbientOcclusion 本来就是线性数据。
// 把两者搞混会让法线贴图被多解一次 gamma，症状是光照方向整体偏移 ——
// 看起来像相机或法线变换写错了，实际只是色彩空间搞错。
struct TextureRecord
{
    asset::ImageData image;
    render::TextureUsage usage = render::TextureUsage::BaseColor;
};

class CpuResourcePool : public render::IRenderDevice, public ResourceView
{
public:
    // ---------------- IRenderDevice ----------------
    MeshHandle createMesh(const asset::MeshData &mesh) override;
    TextureHandle createTexture(const asset::ImageData &image, render::TextureUsage usage) override;
    MaterialHandle createMaterial(const asset::MaterialDesc &material) override;

    void destroy(MeshHandle mesh) override;
    void destroy(TextureHandle texture) override;
    void destroy(MaterialHandle material) override;

    bool updateMeshVertices(MeshHandle mesh, const std::vector<asset::Vertex> &vertices) override;

    // ---------------- ResourceView ----------------
    const asset::MeshData *mesh(MeshHandle handle) const noexcept override;
    const asset::MaterialDesc *material(MaterialHandle handle) const noexcept override;

    // ---------------- 额外查询 ----------------
    const TextureRecord *texture(TextureHandle handle) const noexcept;

    size_t meshCount() const noexcept { return meshes_.size(); }
    size_t textureCount() const noexcept { return textures_.size(); }
    size_t materialCount() const noexcept { return materials_.size(); }

    void clear();

private:
    SlotMap<asset::MeshData, MeshTag> meshes_;
    SlotMap<TextureRecord, TextureTag> textures_;
    SlotMap<asset::MaterialDesc, MaterialTag> materials_;
};

} // namespace my3d::backend::cpu
