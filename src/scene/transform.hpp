#pragma once

// my3d::scene —— 局部变换与层级求值。
//
// 后端无关：只依赖 core/math，绝不 include Filament / SDL。
// 层级求值只实现一次（evaluateWorldTransforms），GPU / ASCII / 未来 Vulkan
// 三种后端共用同一份 —— 这是「一套逻辑驱动多后端」的具体落点之一。

#include "core/handle.hpp"   // kInvalidIndex
#include "core/math.hpp"

#include <cstdint>
#include <vector>

namespace my3d::scene
{

// TRS 变换：位置 + 欧拉角（弧度，YXZ 内旋序）+ 缩放。
//
// 刻意不引入四元数：core/math 目前没有 Quat，而节点局部变换与相机朝向用
// YXZ 欧拉角足以表达。等出现连续旋转插值（骨骼/动画混合）时再补 Quat，
// 现在不付这份成本。
//
// YXZ 序 = R = Ry * Rx * Rz。相机 yaw/pitch 恰好只用 (y, x) 两个分量。
struct Transform
{
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    math::Vec3 eulerRadians{0.0f, 0.0f, 0.0f};
    math::Vec3 scale{1.0f, 1.0f, 1.0f};

    // 直通矩阵：导入器（assimp / glTF）给出的节点局部变换是任意 4x4，可能带
    // 非均匀缩放、镜像（行列式为负）或烘焙进层级的剪切，未必能无损写成 TRS。
    // 置位后 matrix() 直接返回 matrixOverride，上面三个字段被忽略。
    //
    // 为什么不在桥接层「分解成 TRS」把字段全填上：分解在遇到镜像或剪切时会
    // 静默给出错误的旋转分量 —— 世界矩阵错了不会报错，只会让整棵树歪掉，属于
    // 最难查的那类 bug（看起来像相机写错了）。直通没有这个自由度。
    //
    // 反过来，手写场景（程序化几何）继续用 TRS：那里的变换本来就是人摆的，
    // 直通矩阵只会让「改个位置」变成改 16 个浮点数。两种表示并存，各归其位。
    bool matrixOverridden = false;
    math::Mat4 matrixOverride = math::Mat4::identity();

    // T * R * S
    math::Mat4 matrix() const noexcept
    {
        if (matrixOverridden)
            return matrixOverride;

        const math::Mat4 r =
            math::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, eulerRadians.y) *
            math::rotation(math::Vec3{1.0f, 0.0f, 0.0f}, eulerRadians.x) *
            math::rotation(math::Vec3{0.0f, 0.0f, 1.0f}, eulerRadians.z);
        return math::translation(position) * r * math::scaling(scale);
    }
};

// 由 parent 数组推出「父一定排在子之前」的处理顺序。
//
// 为什么不直接假定「父索引 < 子索引」：那只是导入器当前遍历顺序的副产品，
// 换一次遍历（例如广度优先）就会静默算错世界矩阵，而且错得不明显。
// 这里用显式拓扑排序，把注释里的假设变成可检查的事实。
//
// 返回 false 表示层级数据损坏：存在父子环，或 parent 索引越界。
inline bool buildParentFirstOrder(const std::vector<uint32_t> &parent,
                                  std::vector<uint32_t> &order)
{
    constexpr uint8_t kFresh = 0;
    constexpr uint8_t kOnChain = 1;
    constexpr uint8_t kDone = 2;

    const size_t n = parent.size();
    order.clear();
    order.reserve(n);

    std::vector<uint8_t> state(n, kFresh);
    std::vector<uint32_t> chain;

    for (uint32_t start = 0; start < n; ++start)
    {
        if (state[start] != kFresh)
            continue;

        chain.clear();
        uint32_t cur = start;
        while (cur != kInvalidIndex && state[cur] == kFresh)
        {
            state[cur] = kOnChain;
            chain.push_back(cur);

            const uint32_t p = parent[cur];
            if (p == kInvalidIndex)
            {
                cur = kInvalidIndex;
                break;
            }
            if (p >= n)
                return false;   // 越界父索引：数据损坏，不静默当成根
            cur = p;
        }

        // 停在本轮链上的节点 => 父子链绕回了自身
        if (cur != kInvalidIndex && state[cur] == kOnChain)
            return false;

        // 逆序输出：链尾（最靠近根）先出，保证父先于子
        for (size_t i = chain.size(); i-- > 0;)
        {
            order.push_back(chain[i]);
            state[chain[i]] = kDone;
        }
    }
    return true;
}

// world[i] = world[parent[i]] * local[i]（父级变换左乘，点右乘语义）。
// 返回 false 表示层级数据损坏，此时 world 内容未定义。
inline bool evaluateWorldTransforms(const std::vector<math::Mat4> &local,
                                    const std::vector<uint32_t> &parent,
                                    std::vector<math::Mat4> &world)
{
    if (local.size() != parent.size())
        return false;

    std::vector<uint32_t> order;
    if (!buildParentFirstOrder(parent, order))
        return false;

    world.resize(local.size());
    for (uint32_t i : order)
    {
        const uint32_t p = parent[i];
        world[i] = (p == kInvalidIndex) ? local[i] : world[p] * local[i];
    }
    return true;
}

inline bool evaluateWorldTransforms(const std::vector<Transform> &local,
                                    const std::vector<uint32_t> &parent,
                                    std::vector<math::Mat4> &world)
{
    std::vector<math::Mat4> mats(local.size());
    for (size_t i = 0; i < local.size(); ++i)
        mats[i] = local[i].matrix();
    return evaluateWorldTransforms(mats, parent, world);
}

// 自底向上合并子树包围盒：own 是各节点自身网格的包围盒（节点空间），
// 返回值额外含整棵子树。层级损坏时返回空 vector。
//
// 子节点的包围盒在其**自己的**节点空间，并入父节点前必须先用 local[i] 变换过去。
// 漏掉这一步不会报错：只要层级里有一层带缩放（FBX 常见的 cm→m 的 0.01），
// 祖先的包围盒就会被整整放大那个倍数，而包围盒正是取景与剔除的唯一输入 ——
// 表现为「相机摆得极远，模型缩成屏幕上一个点」。
inline std::vector<math::AABB> mergeSubtreeBounds(const std::vector<math::AABB> &own,
                                                  const std::vector<math::Mat4> &local,
                                                  const std::vector<uint32_t> &parent)
{
    std::vector<uint32_t> order;
    if (own.size() != parent.size() || local.size() != parent.size() ||
        !buildParentFirstOrder(parent, order))
        return {};

    std::vector<math::AABB> out = own;
    for (size_t k = order.size(); k-- > 0;)   // 逆序 = 子先于父
    {
        const uint32_t i = order[k];
        const uint32_t p = parent[i];
        if (p != kInvalidIndex)
            out[p].expand(out[i].transformed(local[i]));
    }
    return out;
}

} // namespace my3d::scene
