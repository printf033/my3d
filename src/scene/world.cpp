#include "scene/world.hpp"

#include <algorithm>
#include <utility>

namespace my3d::scene
{

uint32_t World::createNode(const std::string &name)
{
    Node n;
    n.name = name;
    nodes_.push_back(std::move(n));
    return static_cast<uint32_t>(nodes_.size() - 1);
}

Node *World::node(uint32_t index) noexcept
{
    return index < nodes_.size() ? &nodes_[index] : nullptr;
}

const Node *World::node(uint32_t index) const noexcept
{
    return index < nodes_.size() ? &nodes_[index] : nullptr;
}

uint32_t World::parentOf(uint32_t index) const noexcept
{
    return index < nodes_.size() ? nodes_[index].parent : kInvalidIndex;
}

bool World::setParent(uint32_t child, uint32_t parent)
{
    if (child >= nodes_.size())
        return false;
    if (parent != kInvalidIndex && parent >= nodes_.size())
        return false;
    if (parent == child)
        return false;

    // 防环：沿目标父链上溯。若中途遇到 child，说明 child 是 parent 的祖先。
    // 已有数据由本函数维护无环，所以这个循环一定终止。
    for (uint32_t cur = parent; cur != kInvalidIndex; cur = nodes_[cur].parent)
    {
        if (cur == child)
            return false;
    }

    const uint32_t oldParent = nodes_[child].parent;
    if (oldParent != kInvalidIndex)
    {
        auto &siblings = nodes_[oldParent].children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
    }

    nodes_[child].parent = parent;
    if (parent != kInvalidIndex)
        nodes_[parent].children.push_back(child);
    return true;
}

void World::attachPrimitive(uint32_t nodeIndex, const Primitive &primitive)
{
    if (Node *n = node(nodeIndex))
        n->primitives.push_back(primitive);
}

bool World::updateTransforms()
{
    const size_t n = nodes_.size();
    if (n == 0)
    {
        world_.clear();
        return true;
    }

    std::vector<uint32_t> parent(n, kInvalidIndex);
    std::vector<math::Mat4> local(n);
    std::vector<math::AABB> ownBounds(n);

    for (size_t i = 0; i < n; ++i)
    {
        parent[i] = nodes_[i].parent;
        local[i] = nodes_[i].transform.matrix();
        ownBounds[i] = nodes_[i].meshBounds;
    }

    if (!evaluateWorldTransforms(local, parent, world_))
        return false;

    // 子树包围盒在节点空间自底向上合并，再整体变换到世界空间。
    // 变换后的 AABB 会略有放大（重新包围），对剔除是安全的方向。
    const std::vector<math::AABB> subtree = mergeSubtreeBounds(ownBounds, local, parent);
    if (subtree.size() != n)
        return false;

    for (size_t i = 0; i < n; ++i)
    {
        nodes_[i].worldMatrix = world_[i];
        nodes_[i].localBounds = subtree[i];
        nodes_[i].worldBounds = subtree[i].transformed(world_[i]);
    }
    return true;
}

math::AABB World::worldBounds() const noexcept
{
    math::AABB out;
    for (const Node &n : nodes_)
    {
        if (n.parent == kInvalidIndex)
            out.expand(n.worldBounds);
    }
    return out;
}

void World::clear()
{
    nodes_.clear();
    world_.clear();
    lights_.clear();
    environment_ = Environment{};
}

} // namespace my3d::scene
