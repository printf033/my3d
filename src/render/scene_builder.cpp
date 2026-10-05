#include "render/scene_builder.hpp"

#include <cstddef>
#include <utility>

namespace my3d::render
{

SceneBuildResult buildWorldFromAsset(const asset::AssetScene &assetScene,
                                     IRenderDevice &device,
                                     scene::World &world)
{
    SceneBuildResult result;

    if (assetScene.nodes.empty())
    {
        result.error = "asset scene has no nodes (nothing to build)";
        return result;
    }

    // 覆盖语义：先清空。注意 World::clear() 同时清掉 lights 与 environment，
    // 所以调用方必须在 build 之后再加灯，顺序反过来会被静默吃掉。
    world.clear();

    // ------------------------------------------------------------------
    // 后端资源。句柄数组的下标与 AssetScene 里的数组下标一一对应 ——
    // AssetPrimitive 存的就是下标，这层对齐是整个装配的索引基础。
    // ------------------------------------------------------------------
    std::vector<MeshHandle> meshes(assetScene.meshes.size());
    for (size_t i = 0; i < assetScene.meshes.size(); ++i)
    {
        meshes[i] = device.createMesh(assetScene.meshes[i]);
        ++result.stats.meshes;
    }

    std::vector<MaterialHandle> materials(assetScene.materials.size());
    for (size_t i = 0; i < assetScene.materials.size(); ++i)
    {
        materials[i] = device.createMaterial(assetScene.materials[i]);
        ++result.stats.materials;
    }

    // ------------------------------------------------------------------
    // 节点。循环下标即节点索引：World 刚被清空，createNode 从 0 递增，
    // 所以 world 的节点索引与 assetScene.nodes 的下标严格对齐。后面连父子、
    // 挂 primitive 都依赖这一点。
    // ------------------------------------------------------------------
    for (size_t i = 0; i < assetScene.nodes.size(); ++i)
    {
        const asset::AssetNode &src = assetScene.nodes[i];
        const uint32_t index = world.createNode(src.name);
        scene::Node &dst = *world.node(index);

        // 局部变换直通：不试图分解成 TRS（理由见 scene/transform.hpp）。
        dst.transform.matrixOverridden = true;
        dst.transform.matrixOverride = src.localTransform;

        // meshBounds 是「本节点网格的并集，节点空间」—— 与 AssetNode 的语义完全
        // 一致（AssetNode::localBounds 是含子树的，那一个不转写：子树合并由
        // World::updateTransforms 自己重算，两套实现只会互相漂移）。
        dst.meshBounds = src.meshBounds;

        for (const asset::AssetPrimitive &srcPrimitive : src.primitives)
        {
            if (srcPrimitive.mesh >= meshes.size() || srcPrimitive.material >= materials.size())
            {
                // 下标越界说明导入器给出的引用与数组不一致。跳过而不是拿默认值顶上：
                // 渲染一个材质错的网格比缺一块更难查。计数进 stats，让调用方能发现。
                ++result.stats.skippedPrimitives;
                continue;
            }

            scene::Primitive primitive;
            primitive.mesh = meshes[srcPrimitive.mesh];
            primitive.material = materials[srcPrimitive.material];
            // indexOffset / indexCount 保持默认：导入器目前每个 primitive 一段
            // 独立网格，不共享索引缓冲（mesh_data.hpp 的 MeshData 也是一网格一数组）。
            world.attachPrimitive(index, primitive);
            ++result.stats.primitives;
        }

        ++result.stats.nodes;
    }

    // ------------------------------------------------------------------
    // 父子关系。放在所有节点都建好之后：setParent 会拒绝越界索引，而父节点在
    // 数组里可能排在子节点后面（遍历顺序不是拓扑序）。
    // ------------------------------------------------------------------
    for (size_t i = 0; i < assetScene.nodes.size(); ++i)
    {
        const uint32_t parent = assetScene.nodes[i].parent;
        if (parent == kInvalidIndex)
            continue;

        if (!world.setParent(static_cast<uint32_t>(i), parent))
        {
            result.error = "asset scene has an invalid parent link (index " +
                           std::to_string(i) + " -> " + std::to_string(parent) + ")";
            return result;
        }
    }

    // ------------------------------------------------------------------
    // 世界变换求值。失败 = 层级成环（setParent 已挡住一部分，但导入器的数据
    // 仍可能自带环，所以这里必须检查返回值）。
    // ------------------------------------------------------------------
    if (!world.updateTransforms())
    {
        result.error = "world transform evaluation failed (hierarchy contains a cycle)";
        return result;
    }

    if (result.stats.skippedPrimitives > 0)
    {
        result.warnings.push_back("skipped " + std::to_string(result.stats.skippedPrimitives) +
                                  " primitive(s) with out-of-range mesh/material index");
    }

    // 句柄只在成功路径上交出：中途失败时这两个数组虽然未必为空，但已经建好的资源
    // 未必对应一个可渲染的 world（失败点之后可能还有没建的）。让调用方拿不到
    // 半成品，比让它拿着一份看着像真的、实则不全的数组去按索引取材要安全。
    result.meshHandles = std::move(meshes);
    result.materialHandles = std::move(materials);

    result.ok = true;
    return result;
}

} // namespace my3d::render
