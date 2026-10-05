#pragma once

// my3d::asset —— 骨骼动画：通道、关键帧、采样、蒙皮。
//
// 三条边界，刻意划清：
//
//   1. 不认识 assimp。导入器把 aiAnimation 翻译成下面的结构，翻译完这一层与上游
//      无关 —— 换成 glTF 直读或自研格式，改动不出现在这里。
//   2. 不持有时钟。「现在几点」由调用方决定，采样函数收的就是一个绝对的秒数。
//      播放速度、暂停、拖拽、循环语义全部留在调用方，采样器对时间没有语义。
//   3. 不碰后端。蒙皮把结果写进调用方给的顶点数组，谁把它上传到哪儿与这一层无关。
//      也正因如此，core 的「零第三方依赖」约束在这里不被打破 —— 这个文件不 include
//      任何第三方头。
//
// 结构上刻意贴近 assimp 的 aiNodeAnim（位置/旋转/缩放三组关键帧挂在同一个通道
// 上），而不是 glTF 的「一个属性一个通道」：assimp 已是唯一输入，glTF 的拆分在
// 它内部被合并过一次，在这里再拆回去只多一次结构转换，不产生任何新信息。

#include "core/handle.hpp"
#include "core/math.hpp"
#include "mesh_data.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace my3d::asset
{

// 单个关键帧：时刻（秒）+ 值。
//
// 三组关键帧各自一条时间轴，不共用。理由：aiNodeAnim 的 mPositionKeys /
// mRotationKeys / mScalingKeys 各带自己的 mTime，实测（assets/model/mia）中长度
// 常常不同 —— 纯旋转骨骼只有 1 个 position 关键帧。强行合并成一条时间轴就必须
// 重采样，而重采样会引入原本不存在的插值误差。
struct Vec3Key
{
    float time = 0.0f;
    math::Vec3 value{};
};

struct QuatKey
{
    float time = 0.0f;

    // 默认单位四元数，不是零四元数。零四元数不是旋转：一旦某个关键帧漏填，
    // 症状是整个节点塌成一个点，而不是「转了 0 度」—— 前者一眼可见，后者会
    // 悄悄混进插值结果里。
    math::Quat value = math::Quat::identity();
};

// 一个动画通道：目标节点 + 三组关键帧。
//
// 目标同时保留名字与索引：名字是资产里的事实（报错、跨文件对照都要它），索引是
// 运行期的快速通道。导入器负责把名字解析成索引并校验，运行期只看索引。
struct AnimationChannel
{
    std::string nodeName;
    uint32_t node = kInvalidIndex;   // 解析失败保持 kInvalidIndex，采样方应跳过

    std::vector<Vec3Key> positions;
    std::vector<QuatKey> rotations;
    std::vector<Vec3Key> scales;

    bool drivesPosition() const noexcept { return !positions.empty(); }
    bool drivesRotation() const noexcept { return !rotations.empty(); }
    bool drivesScale() const noexcept { return !scales.empty(); }
};

struct AnimationClip
{
    std::string name;
    float durationSeconds = 0.0f;
    std::vector<AnimationChannel> channels;
};

// 定位 t 落在哪个区间：返回满足 keys[i].time <= t 的最大 i。
//
// 每次采样重新二分，不缓存游标。老实现（my3Ddel 的 KeyFrame::getPositionIndex）
// 用一个只增不减的游标缓存命中位置，动画回绕到第二遍时游标不回头，索引落在错误
// 的区间上 —— 症状是「第一遍正常，第二遍开始抽搐」。二分是 O(log n)，通道只有
// 几十条，这份稳定比省下的几次比较值钱得多。
template <typename Key>
size_t locateKey(const std::vector<Key> &keys, float t) noexcept
{
    const auto it = std::upper_bound(keys.begin(), keys.end(), t,
                                     [](float v, const Key &k) { return v < k.time; });
    if (it == keys.begin())
        return 0;
    return static_cast<size_t>(it - keys.begin()) - 1u;
}

// 在一组关键帧上采样，blend(a, b, f) 给出 a→b 之间 f 处的值。
//
// 边界语义：t 早于首帧取首帧，晚于末帧取末帧（夹取，不外插）。末帧之后不是错误 ——
// 通道可以短于 clip 时长（骨骼动画里很常见），夹取才是正确行为。
template <typename Key, typename Value, typename Blend>
Value sampleKeyRange(const std::vector<Key> &keys, float t, const Value &fallback, Blend blend)
{
    if (keys.empty())
        return fallback;
    if (t <= keys.front().time)
        return keys.front().value;
    if (t >= keys.back().time)
        return keys.back().value;

    const size_t i = locateKey(keys, t);
    const Key &a = keys[i];
    const Key &b = keys[i + 1];   // 上面的夹取保证 i + 1 在范围内

    const float span = b.time - a.time;
    if (!(span > 0.0f))
        return b.value;   // 时间戳重复：取后者，不产生 0/0

    return blend(a.value, b.value, (t - a.time) / span);
}

// 一个通道在 t 时刻采样出的局部姿势。
//
// 三个分量各自带「本次是否被通道驱动」的标记。实测的资产里三项都为 true（assimp
// 给三组都填了至少一个关键帧），标记存在是为了让「通道只驱动一部分」这件事**可见**
// —— 调用方据此决定是跳过还是回落到绑定姿态，而不是拿零位移/单位缩放去凑一个
// 看起来能动、其实丢了一个分量的姿势。
struct ChannelPose
{
    bool hasPosition = false;
    bool hasRotation = false;
    bool hasScale = false;

    math::Vec3 position{};
    math::Quat rotation = math::Quat::identity();
    math::Vec3 scale{1.0f, 1.0f, 1.0f};

    bool complete() const noexcept { return hasPosition && hasRotation && hasScale; }

    // 场景层可直接使用的局部矩阵。
    //
    // 通道里的 TRS 是资产原样值，my3d 的 math::compose 就是列向量约定下的
    // M = T * R * S，直接产出即可，不要再转置。
    //
    // 这里曾经套过一次 math::transpose，依据是「assimp 用行向量约定、toMat4
    // 产出的是 M^T」，所以通道也得跟着转一次才对得上。那个前提是错的：
    // importer.cpp 的 toMat4 实测下来是逐元素直搬（见那里的判据），于是两个
    // 落点必须一起改 —— 只改一处，动画路径与静态路径就落到不同约定里，
    // 症状是「静态模型正常，一播动画整具甩到屏幕外」。
    math::Mat4 matrix() const noexcept
    {
        return math::compose(position, rotation, scale);
    }
};

// 在 t 时刻采样一个通道。未被驱动的分量保持中性值并把对应标记置 false。
ChannelPose sampleChannel(const AnimationChannel &channel, float t);

// ---------------- 蒙皮 ----------------

// 每个骨骼槽位的蒙皮矩阵：jointMatrix[j] = nodeWorld[jointNodes[j]] * inverseBind[j]。
//
// 顺序不能反。inverseBind 把顶点从网格空间搬到骨骼的绑定空间，nodeWorld 再把它
// 从绑定空间搬到当前姿势 —— 写反了得到的矩阵在 t=0 时**恰好是 identity 附近**，
// 看起来「差不多对」，一播起来整具模型被甩到屏幕外。nodeWorld 的长度必须覆盖
// jointNodes 里的最大索引，否则该槽位写成 identity 并在 outInvalid 里计数。
void computeJointMatrices(const MeshData &mesh, const std::vector<math::Mat4> &nodeWorld,
                          std::vector<math::Mat4> &outJointMatrices,
                          size_t *outInvalidJoints = nullptr);

// 线性混合蒙皮：从 mesh.bindVertices 重算出 out 的 position / normal / tangent。
//
// 法线用同一组权重混合后再归一化（不做逆置转置）。理由：蒙皮矩阵由旋转主导，
// 均匀缩放的骨骼上两者等价；非均匀缩放骨骼下严格正确的做法是每骨骼求逆转置，
// 代价是每顶点 4 次 3x3 求逆 —— 收益在 ASCII 分辨率下不可见。
//
// 权重全零的顶点（未被任何骨骼覆盖）原样保留 bind 位置，而不是塌到原点：前者
// 是「这块没绑上」，后者是「整块飞走」。UV 与其它属性不参与变形，直接复制。
//
// out 会被 resize 到 mesh.vertices.size()，可复用同一块缓冲以避免每帧分配。
void skinVertices(const MeshData &mesh, const std::vector<math::Mat4> &jointMatrices,
                  std::vector<Vertex> &out);

} // namespace my3d::asset
