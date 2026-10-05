#pragma once

// 模型导入：assimp → asset 层数据（AssetScene）。
//
// 这是全项目唯一 include <assimp/> 的公开入口。scene / render / backend
// 都不认识 assimp 类型。
//
// 与旧 engine.hpp::importMesh 的关键差异：
//   * 不用 assert 做错误处理（assert 在 release 下消失，旧代码那些 assert(textures)
//     之后紧跟的是越界访问）；
//   * 材质默认值不再依赖未初始化的栈变量（旧代码 `float roughnessFactor;`
//     在 Get 失败时会读到垃圾值）；
//   * 每个 mesh 独立成 MeshData，而不是把整个模型塞进一张全局大缓冲，
//     后端仍可自行合并；
//   * 缺失/降级逐项记进 warnings，不静默画错。

#include "asset_scene.hpp"
#include "ibl.hpp"
#include "material.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::asset
{

struct ImportOptions
{
    // 所有材质的默认着色模型。后端据此挑自己的实现
    // （Filament 后端映射到 lit / lit_aorm / unlit 三套 .filamat；
    //  CPU 后端映射到不同的着色分支）。
    // 核心层只认识这个枚举，不认识 .filamat、SPIR-V 或任何 shader 产物。
    ShadingModel shadingModel = ShadingModel::Lit;

    // 外部贴图搜索目录。留空则用 "<模型目录>/textures/"（与旧代码一致）。
    std::string textureDirectory;

    // emissive 强度的额外乘子，叠加在 AI_MATKEY_EMISSIVE_INTENSITY 之上。
    float emissiveScale = 1.0f;

    // 单张贴图最长边上限，超出则 box 降采样；0 表示不限制。
    // CPU / ASCII 后端靠它把纹理内存压进合理范围。
    uint32_t maxTextureSize = 0;
};

struct ImportStats
{
    uint32_t nodeCount = 0;
    uint32_t meshCount = 0;
    uint32_t materialCount = 0;
    uint32_t textureCount = 0;
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;

    // 蒙皮与动画。skinnedMeshCount 数的是「带骨骼权重、可参与蒙皮」的网格；
    // animationCount 是文件里的动画条数 —— 导入器全部保留，不做按名合并。
    uint32_t skinnedMeshCount = 0;
    uint32_t animationCount = 0;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;   // 每一项降级/缺失都在这里留痕
    AssetScene scene;
    ImportStats stats;
};

// 导入模型。失败时 ok == false 且 error 非空，scene 内容不保证可用。
// 单张贴图加载失败只记 warning —— 模型其余部分照常导入。
ImportResult importScene(const std::string &path, const ImportOptions &options = {});

} // namespace my3d::asset
