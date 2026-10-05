#pragma once

// my3d::render —— 骨骼动画驱动：把一条 AnimationClip 在某个时刻的姿势写回
// scene::World，并把蒙皮结果经 IRenderDevice 送到后端。
//
// 它坐在「不认识后端的 asset」与「不认识骨骼的后端」中间：
//
//     asset/animation.*          采样与蒙皮，纯数学，零依赖
//     render/animation_driver.*  ← 这一层：绑定到 World、通过 IRenderDevice 上传
//     backend/*                  只管画当前顶点
//
// 为什么单拎一层而不写在样例里：样例里再写一遍「遍历通道、查节点、求世界矩阵、
// 算关节矩阵、上传」，两个样例就是两份实现，随即开始漂移。ASCII 与 Filament
// 共享的必须是这一段。

#include "asset/animation.hpp"
#include "asset/asset_scene.hpp"
#include "core/handle.hpp"
#include "render/renderer.hpp"
#include "scene/world.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::render
{

class AnimationDriver
{
public:
    struct Stats
    {
        uint32_t clipChannels = 0;        // 这条 clip 的通道总数
        uint32_t resolvedChannels = 0;    // 成功落到 World 节点上的通道数
        uint32_t unresolvedChannels = 0;  // 名字在节点表里找不到的通道数
        uint32_t duplicateChannels = 0;   // 同一节点被两条通道驱动（资产可疑）
        uint32_t skinnedMeshes = 0;       // 需要每帧重算的网格数
        uint32_t skinnedVertices = 0;     // 这些网格的顶点总数

        // 下面四项是最近一次 apply() 的观测值；bind 之后、首次 apply 之前为 0。
        uint32_t invalidJoints = 0;    // 引用越界节点的骨骼槽位数
        uint32_t uploadedMeshes = 0;   // 成功送到后端的网格数
        uint32_t failedUploads = 0;    // 后端拒绝或句柄缺失的网格数
        uint32_t uncompensatedMeshes = 0;  // 没有所属节点、无法做坐标系补偿的网格数
    };

    // 诊断：每个被蒙皮网格当前顶点缓冲的包围盒，以及它被补偿到的节点。
    //
    // 「画面上模型大小或位置不对」这类问题在终端里没有中间态可看 —— 要么对，
    // 要么一团。把每个网格的实际包围盒打出来，才能把「补偿用错了节点」
    // 「某个网格被多个节点引用」这些假设逐个证实或证伪。只读，不改任何状态。
    struct MeshBounds
    {
        uint32_t meshIndex = kInvalidIndex;
        uint32_t nodeIndex = kInvalidIndex;
        uint32_t ownerCount = 0;  // 引用该网格的节点数；>1 时一份顶点缓冲服务不了全部变换
        math::AABB box;           // 当前顶点（补偿后），网格节点局部空间
        math::AABB bindBox;       // bindVertices 的包围盒：导入器交出来的原始空间
        math::Vec3 nodeScale{};   // 所属节点世界矩阵的三轴缩放，没有节点时为 0
    };
    std::vector<MeshBounds> meshBoundsDiag(const scene::World &world) const;

    // 绑定：解析「通道 → 节点」，并**拷走**需要蒙皮的网格。
    //
    // 拷贝是刻意的。驱动每帧要写 vertices，而它绝不能去改 asset::AssetScene：
    // 那份数据是绑定姿态的唯一来源（bindVertices 就在里面），被写脏之后
    // 「回落到绑定姿态」这条退路就没了。拷贝之后驱动与调用方零共享所有权。
    //
    // 返回 false 时 error() 给出原因。资产没有这条 clip、通道一个都没对上、
    // 或没有任何网格可蒙皮，都算绑定失败 —— 静默成功会让样例安静地放一段
    // 没有动画的动画，而那看起来和「动画做错了」一模一样。
    bool bind(const asset::AssetScene &scene, uint32_t clipIndex = 0);
    bool bind(const asset::AssetScene &scene, const std::string &clipName);

    bool bound() const noexcept { return m_bound; }
    uint32_t clipIndex() const noexcept { return m_clipIndex; }
    float duration() const noexcept { return m_duration; }
    const std::string &clipName() const noexcept { return m_clipName; }
    const Stats &stats() const noexcept { return m_stats; }
    const std::string &error() const noexcept { return m_error; }

    // 采样 timeSeconds（对 duration 取模 = 循环播放），写入 World 的节点局部变换，
    // 求值世界矩阵，再把每个蒙皮网格的新顶点上传给 device。
    //
    // 依赖一条不变量：World 的节点索引与 AssetScene::nodes 的下标严格对齐 ——
    // 这正是 buildWorldFromAsset 的保证（见 scene_builder.cpp 的注释）。不满足时
    // 在 error() 里说明并返回 false，而不是把姿势写到别的节点上。
    //
    // 该函数会**覆盖**这些节点的局部变换（matrixOverride）。动画拥有它所驱动的
    // 节点，调用方不应在动画期间再手改同一批节点的 transform。
    bool apply(float timeSeconds, scene::World &world, IRenderDevice &device,
               const std::vector<MeshHandle> &meshHandles);

    // 蒙皮内核自检：把所有关节矩阵设为单位阵重蒙一遍，结果必须逐顶点等于绑定
    // 姿态。返回最大位置偏差（模型单位），未绑定或无蒙皮网格时返回 -1。
    //
    // 这一项与动画数据无关，它单拎出 skinVertices + skinAttributes + 权重这一
    // 段。那一段一旦有偏差，任何姿势都会带上同一份偏差，画面上只表现为「整体
    // 轻微变形」—— 肉眼认不出来，但整套姿势都是错的；而 bind-pose 下恰好为零
    // 偏差是个精确到浮点的硬判据。
    float skinningKernelError() const;

private:
    void clearBinding(uint32_t clipIndex);

    struct Skinned
    {
        uint32_t meshIndex = kInvalidIndex;
        // 拥有这个网格的节点索引（没有节点引用时为 kInvalidIndex）。apply 用它取
        // 当前的世界矩阵做坐标系补偿 —— 渲染器的 item.worldTransform 读的是同一个
        // 矩阵，两边用同一份才能严格抵消。
        uint32_t nodeIndex = kInvalidIndex;
        uint32_t ownerCount = 0;
        asset::MeshData mesh;                   // 绑定姿态 + 蒙皮属性（私有副本）
        std::vector<math::Mat4> jointMatrices;  // 每帧复用，避免反复分配
    };

    bool m_bound = false;
    std::string m_error;
    std::string m_clipName;
    uint32_t m_clipIndex = 0;
    float m_duration = 0.0f;
    Stats m_stats;

    std::vector<asset::AnimationChannel> m_channels;   // 只保留解析成功的通道
    std::vector<uint32_t> m_channelOfNode;             // 节点索引 → 通道下标
    std::vector<math::Mat4> m_bindLocal;               // 节点索引 → 绑定姿态局部矩阵
    std::vector<Skinned> m_skinned;
};

} // namespace my3d::render
