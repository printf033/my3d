#include "asset/asset_scene.hpp"

namespace my3d::asset
{

// 依赖 importer 的保证：节点数组按「父节点先于子节点」的 DFS 顺序排列，
// 因此一趟正向遍历就能算完所有世界变换。
std::vector<math::Mat4> AssetScene::worldTransforms() const
{
    std::vector<math::Mat4> out(nodes.size(), math::Mat4::identity());
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const AssetNode &node = nodes[i];
        const bool hasParent = node.parent != kInvalidIndex && node.parent < nodes.size() && node.parent != i;
        out[i] = hasParent ? out[node.parent] * node.localTransform : node.localTransform;
    }
    return out;
}

} // namespace my3d::asset
