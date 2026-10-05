#include "backend/cpu/resource_pool.hpp"

namespace my3d::backend::cpu
{

MeshHandle CpuResourcePool::createMesh(const asset::MeshData &mesh)
{
    // 存一份拷贝而不是指针。
    //
    // CPU 后端的资源生命周期独立于调用方的临时对象 —— 场景加载完可以释放
    // 原始 MeshData，设备里的副本必须继续有效。句柄 + 拷贝是所有后端都能
    // 遵守的最简单契约；引用语义会立刻引出「谁负责活多久」的问题，而这个问题
    // 在 Filament 后端有完全不同的答案。
    return meshes_.insert(mesh);
}

TextureHandle CpuResourcePool::createTexture(const asset::ImageData &image,
                                             render::TextureUsage usage)
{
    return textures_.insert(TextureRecord{image, usage});
}

MaterialHandle CpuResourcePool::createMaterial(const asset::MaterialDesc &material)
{
    return materials_.insert(material);
}

void CpuResourcePool::destroy(MeshHandle meshHandle) { meshes_.erase(meshHandle); }
void CpuResourcePool::destroy(TextureHandle textureHandle) { textures_.erase(textureHandle); }
void CpuResourcePool::destroy(MaterialHandle materialHandle) { materials_.erase(materialHandle); }

bool CpuResourcePool::updateMeshVertices(MeshHandle handle, const std::vector<asset::Vertex> &vertices)
{
    asset::MeshData *mesh = meshes_.get(handle);
    if (mesh == nullptr)
        return false;

    // 顶点数变了就不是「原地更新」而是换一份数据：索引缓冲会因此失效，必须走
    // destroy + createMesh 重建。CPU 后端自己其实能容下 resize，但接口契约一旦
    // 放宽到「可以改顶点数」，每个后端都得为索引失效做额外处理，而没有任何调用方
    // 需要这个能力。拒绝在这里是廉价的，事后查「为什么三角形乱跳」是昂贵的。
    if (mesh->vertices.size() != vertices.size())
        return false;

    mesh->vertices = vertices;
    return true;
}

const asset::MeshData *CpuResourcePool::mesh(MeshHandle handle) const noexcept
{
    return meshes_.get(handle);
}

const asset::MaterialDesc *CpuResourcePool::material(MaterialHandle handle) const noexcept
{
    return materials_.get(handle);
}

const TextureRecord *CpuResourcePool::texture(TextureHandle handle) const noexcept
{
    return textures_.get(handle);
}

void CpuResourcePool::clear()
{
    meshes_.clear();
    textures_.clear();
    materials_.clear();
}

} // namespace my3d::backend::cpu
