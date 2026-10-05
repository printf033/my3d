#pragma once

// 导入后的「素材场景」：纯数据，不含句柄，不含后端知识。
// 它是 importer 的输出，也是 scene::World 的输入。

#include "animation.hpp"
#include "core/handle.hpp"
#include "core/math.hpp"
#include "image_data.hpp"
#include "material.hpp"
#include "mesh_data.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::asset
{

// 一个可绘制段落：一段 mesh 配一个材质。
struct AssetPrimitive
{
    uint32_t mesh = kInvalidIndex;
    uint32_t material = kInvalidIndex;
};

// 场景节点。
//
// 旧 Node 是 unordered_map<string, Node> 的递归嵌套，名字既是查找键又是身份，
// 重名会静默互相覆盖。这里用扁平数组 + 索引表达父子关系：值语义、可整体复制、
// 无递归析构、遍历缓存友好，也便于将来序列化。
struct AssetNode
{
    std::string name;
    math::Mat4 localTransform = math::Mat4::identity();
    math::AABB meshBounds;      // 仅本节点网格的并集（节点空间）
    math::AABB localBounds;     // 含整棵子树（节点空间），剔除用
    std::vector<AssetPrimitive> primitives;
    std::vector<uint32_t> children;
    uint32_t parent = kInvalidIndex;
};

struct AssetScene
{
    std::string sourcePath;
    std::vector<AssetNode> nodes;
    uint32_t root = kInvalidIndex;
    std::vector<MeshData> meshes;
    std::vector<MaterialDesc> materials;
    std::vector<ImagePtr> images;    // 去重后的纹理池（也与 materials 里的 slot 共享）
    std::vector<AnimationClip> animations;   // 全部保留，不按名字合并
    math::AABB bounds;               // 世界空间包围盒

    bool empty() const noexcept { return nodes.empty(); }

    // 各节点 world transform 是 localTransform 连乘的结果，导入期只算一次。
    std::vector<math::Mat4> worldTransforms() const;
};

} // namespace my3d::asset
