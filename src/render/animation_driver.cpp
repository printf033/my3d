#include "render/animation_driver.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace my3d::render
{

bool AnimationDriver::bind(const asset::AssetScene &scene, uint32_t clipIndex)
{
    clearBinding(clipIndex);

    if (clipIndex >= scene.animations.size())
    {
        m_error = "asset scene has no animation clip #" + std::to_string(clipIndex) +
                  " (it has " + std::to_string(scene.animations.size()) + ")";
        return false;
    }
    if (scene.nodes.empty())
    {
        m_error = "asset scene has no nodes to animate";
        return false;
    }

    const asset::AnimationClip &clip = scene.animations[clipIndex];
    m_clipName = clip.name;
    m_duration = clip.durationSeconds;
    m_stats.clipChannels = static_cast<uint32_t>(clip.channels.size());

    // 节点名 → 索引。重名保留第一个：资产里重名本身就可疑（assimp 的节点名理应
    // 唯一，实测 mia 的 98 个节点零重名），但真撞上时「先出现的赢」是可预测的，
    // 而「后出现的覆盖」会把通道接到一个更深的同名节点上，姿势错得没有线索。
    std::unordered_map<std::string, uint32_t> nodeIndex;
    nodeIndex.reserve(scene.nodes.size() * 2);
    for (uint32_t i = 0; i < scene.nodes.size(); ++i)
    {
        const std::string &name = scene.nodes[i].name;
        if (!name.empty())
            nodeIndex.emplace(name, i);
    }

    // 通道 → 节点。
    m_channelOfNode.assign(scene.nodes.size(), kInvalidIndex);
    m_channels.reserve(clip.channels.size());
    for (const asset::AnimationChannel &channel : clip.channels)
    {
        const auto it = nodeIndex.find(channel.nodeName);
        if (it == nodeIndex.end())
        {
            ++m_stats.unresolvedChannels;
            continue;
        }

        const uint32_t node = it->second;
        if (m_channelOfNode[node] != kInvalidIndex)
        {
            // 一个节点两条通道：保留先出现的那条。assimp 的 aiNodeAnim 是一个节点
            // 一条，真出现重复说明资产（或导入器）有问题，计数让上层看得见。
            ++m_stats.duplicateChannels;
            continue;
        }

        m_channelOfNode[node] = static_cast<uint32_t>(m_channels.size());
        m_channels.push_back(channel);
    }
    m_stats.resolvedChannels = static_cast<uint32_t>(m_channels.size());

    if (m_channels.empty())
    {
        m_error = "animation clip '" + m_clipName +
                  "' has no channel matching any node name (channels " +
                  std::to_string(m_stats.clipChannels) + ", nodes " +
                  std::to_string(scene.nodes.size()) + ")";
        return false;
    }

    // 绑定姿态的局部矩阵。未被通道驱动的节点每帧回落到这里 —— 见 apply()。
    m_bindLocal.resize(scene.nodes.size());
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        m_bindLocal[i] = scene.nodes[i].localTransform;

    // 网格 → 拥有它的节点。“谁能拥有”这一问在这里才回答得了：AssetScene 的引用
    // 方向是「节点 → primitive → 网格下标」，反向映射必须扫一遍。
    //
    // 一个网格被多个节点引用时取先出现的那个：buildWorldFromAsset 对同一个 mesh
    // 下发同一个句柄，顶点缓冲只有一份，所以「哪个节点拥有它」只影响补偿用的
    // 世界矩阵 —— 取先出现的与句柄共享的语义一致。若两个节点的世界变换不同，
    // 这个网格本来就不可能同时对（一份缓冲没法服务两个变换），不是这里该遮的事。
    std::vector<uint32_t> meshOwner(scene.meshes.size(), kInvalidIndex);
    std::vector<uint32_t> meshRefs(scene.meshes.size(), 0);
    for (uint32_t n = 0; n < scene.nodes.size(); ++n)
    {
        for (const asset::AssetPrimitive &primitive : scene.nodes[n].primitives)
        {
            if (primitive.mesh >= meshOwner.size())
                continue;
            ++meshRefs[primitive.mesh];
            if (meshOwner[primitive.mesh] == kInvalidIndex)
                meshOwner[primitive.mesh] = n;
        }
    }

    // 只拷可蒙皮的网格。非蒙皮网格的顶点是常量，每帧上传只是白白占带宽，这条判断
    // 也让程序化资产（viewer_ascii 的立方体 + 地面）完全不受这条路径影响。
    for (uint32_t m = 0; m < scene.meshes.size(); ++m)
    {
        const asset::MeshData &mesh = scene.meshes[m];
        if (!mesh.skinned())
            continue;

        Skinned entry;
        entry.meshIndex = m;
        entry.nodeIndex = meshOwner[m];
        entry.ownerCount = meshRefs[m];
        entry.mesh = mesh;   // 深拷贝：bindVertices / skinAttributes / jointNodes 都在内
        m_skinned.push_back(std::move(entry));

        ++m_stats.skinnedMeshes;
        m_stats.skinnedVertices += mesh.vertexCount();
    }

    if (m_skinned.empty())
    {
        m_error = "animation clip '" + m_clipName +
                  "' is bound but no mesh is skinnable (missing bindVertices / "
                  "skinAttributes / jointNodes on every mesh)";
        return false;
    }

    m_bound = true;
    return true;
}

bool AnimationDriver::bind(const asset::AssetScene &scene, const std::string &clipName)
{
    clearBinding(0);

    uint32_t match = kInvalidIndex;
    for (uint32_t i = 0; i < scene.animations.size(); ++i)
    {
        if (scene.animations[i].name != clipName)
            continue;

        if (match != kInvalidIndex)
        {
            m_error = "animation clip name '" + clipName + "' is ambiguous";
            return false;
        }
        match = i;
    }

    if (match == kInvalidIndex)
    {
        m_error = "asset scene has no animation clip named '" + clipName + "'";
        if (!scene.animations.empty())
        {
            m_error += " (available:";
            for (uint32_t i = 0; i < scene.animations.size(); ++i)
            {
                const std::string &name = scene.animations[i].name;
                m_error += " '" + (name.empty() ? "clip #" + std::to_string(i) : name) + "'";
            }
            m_error += ")";
        }
        return false;
    }

    return bind(scene, match);
}

void AnimationDriver::clearBinding(uint32_t clipIndex)
{
    // 全部重置：bind 可以重复调用（换一条 clip），旧状态不能漏给新绑定。
    m_bound = false;
    m_error.clear();
    m_clipName.clear();
    m_clipIndex = clipIndex;
    m_duration = 0.0f;
    m_stats = Stats{};
    m_channels.clear();
    m_channelOfNode.clear();
    m_bindLocal.clear();
    m_skinned.clear();
}

bool AnimationDriver::apply(float timeSeconds, scene::World &world, IRenderDevice &device,
                            const std::vector<MeshHandle> &meshHandles)
{
    m_error.clear();

    if (!m_bound)
    {
        m_error = "AnimationDriver::apply called before a successful bind()";
        return false;
    }
    if (world.nodeCount() != m_bindLocal.size())
    {
        m_error = "world has " + std::to_string(world.nodeCount()) + " nodes but the asset has " +
                  std::to_string(m_bindLocal.size()) +
                  "; the driver needs buildWorldFromAsset's 1:1 node correspondence";
        return false;
    }

    // 循环语义留在这一层：asset 的采样器对时间没有语义，它收的只是一个秒数。
    // 取模放在这里而不是交给调用方，是因为「播到末尾要回绕」是驱动唯一的默认语义，
    // 调用方漏掉它只会得到一具停在末帧不动的模型。
    float t = timeSeconds;
    if (m_duration > 0.0f)
    {
        t = std::fmod(t, m_duration);
        if (!(t >= 0.0f))   // fmod 对负输入返回负值；NaN 也走这条
            t += m_duration;
    }

    for (size_t i = 0; i < m_bindLocal.size(); ++i)
    {
        scene::Node *node = world.node(static_cast<uint32_t>(i));
        if (node == nullptr)
        {
            m_error = "world node " + std::to_string(i) + " is null";
            return false;
        }

        // 默认回落绑定姿态：实测 mia 的 61 根骨骼里有 23 根没有任何通道
        // （thigh_twist 之类的辅助骨）。不写这一行，它们会保持上一次 apply 留下的
        // 姿势 —— 症状是每帧漂移一点点，几十秒后被拉成一根面条。
        math::Mat4 local = m_bindLocal[i];
        const uint32_t channel = m_channelOfNode[i];
        if (channel != kInvalidIndex)
            local = asset::sampleChannel(m_channels[channel], t).matrix();

        node->transform.matrixOverridden = true;
        node->transform.matrixOverride = local;
    }

    // 走 World 自己的求值，而不是自己调 evaluateWorldTransforms：它同时更新
    // worldBounds，而剔除读的正是那一份 —— 骨骼把模型抬到天上之后，包围盒不跟
    // 就会整块被剔掉。
    if (!world.updateTransforms())
    {
        m_error = "world transform evaluation failed while applying the animation";
        return false;
    }

    const std::vector<math::Mat4> &nodeWorld = world.worldMatrices();

    m_stats.invalidJoints = 0;
    m_stats.uploadedMeshes = 0;
    m_stats.failedUploads = 0;
    m_stats.uncompensatedMeshes = 0;

    for (Skinned &skinned : m_skinned)
    {
        size_t invalid = 0;
        asset::computeJointMatrices(skinned.mesh, nodeWorld, skinned.jointMatrices, &invalid);
        m_stats.invalidJoints += static_cast<uint32_t>(invalid);

        // ------------------------------------------------------------------
        // 坐标系补偿。skinVertices 的输出在世界空间 —— 这是 inverseBind 的语义
        // 决定的：导入器把 assimp 的 offset 原样带过来，它等于
        // inverse(绑定时的关节世界矩阵) * 绑定时的网格世界矩阵，那个右侧因子
        // 正是一份厘米→米的换算，所以绑定姿态下关节矩阵不是单位阵而是一个
        // 缩小项，蒙皮结果直接落在世界空间。
        //
        // 而渲染路径拿到 vertices 后还会乘一次 item.worldTransform
        // （toolbox.cpp: item.worldTransform = n.worldMatrix），于是缩放生效两次。
        // mia 实测 0.01² 之后整个模型缩到毫米级，看起来就像「动画播不出来」。
        //
        // 修法选在矩阵而非顶点上：左乘同一个 nodeWorld 的逆，把补偿折进关节矩阵。
        // 三个好处 ——
        //   1) 代价 O(骨骼数) 而不是 O(顶点数)；
        //   2) 与渲染器读的是同一个矩阵，数学上严格抵消，不依赖两处各写一遍；
        //   3) 语义回到「网格节点局部空间」，与 meshBounds / worldBounds 的剔除、
        //      以及其它后端（含 Filament 的 model matrix 路径）全部一致。
        //
        // 缺所属节点时不猜：坐标系无法保证正确的网格不上传，计数让上层看得见。
        // ------------------------------------------------------------------
        if (skinned.nodeIndex >= nodeWorld.size())
        {
            ++m_stats.uncompensatedMeshes;
            continue;
        }
        const math::Mat4 toLocal = math::inverse(nodeWorld[skinned.nodeIndex]);
        for (math::Mat4 &joint : skinned.jointMatrices)
            joint = toLocal * joint;

        // 从 bindVertices 重算，而不是在上一帧的 vertices 上接着变形：后者会把
        // 浮点误差逐帧累加，几十秒后模型开始发飘。重算天然幂等，也天然支持回落到
        // 绑定姿态（见 mesh_data.hpp 里 vertices / bindVertices 的分工）。
        //
        // 别名说明：第三参就是 skinned.mesh 自己的 vertices，与它读的 bindVertices
        // 是两个不同的数组，skinVertices 也只读后者 —— 这是它作为「可复用输出缓冲」
        // 的用法，不会自我覆盖。
        asset::skinVertices(skinned.mesh, skinned.jointMatrices, skinned.mesh.vertices);

        if (skinned.meshIndex >= meshHandles.size())
        {
            ++m_stats.failedUploads;
            continue;
        }

        if (device.updateMeshVertices(meshHandles[skinned.meshIndex], skinned.mesh.vertices))
            ++m_stats.uploadedMeshes;
        else
            ++m_stats.failedUploads;
    }

    return true;
}

float AnimationDriver::skinningKernelError() const
{
    if (m_skinned.empty())
        return -1.0f;

    std::vector<asset::Vertex> skinnedVertices;
    std::vector<math::Mat4> identityJoints;
    float worst = 0.0f;

    for (const Skinned &entry : m_skinned)
    {
        const asset::MeshData &mesh = entry.mesh;
        if (mesh.bindVertices.empty() || mesh.jointNodes.empty())
            continue;

        // 单位关节矩阵 = 「每根骨骼都停在绑定姿态」：jointMatrix 退化为单位阵，
        // 于是蒙皮结果必须逐顶点等于 bindVertices，不引入任何缩放或旋转。
        identityJoints.assign(mesh.jointNodes.size(), math::Mat4::identity());
        asset::skinVertices(mesh, identityJoints, skinnedVertices);

        const size_t count = std::min(skinnedVertices.size(), mesh.bindVertices.size());
        for (size_t i = 0; i < count; ++i)
        {
            const math::Vec3 delta = skinnedVertices[i].position - mesh.bindVertices[i].position;
            worst = std::max(worst, math::length(delta));
        }
    }

    return worst;
}

std::vector<AnimationDriver::MeshBounds>
AnimationDriver::meshBoundsDiag(const scene::World &world) const
{
    std::vector<MeshBounds> out;
    out.reserve(m_skinned.size());

    const std::vector<math::Mat4> &nodeWorld = world.worldMatrices();

    for (const Skinned &entry : m_skinned)
    {
        MeshBounds bounds;
        bounds.meshIndex = entry.meshIndex;
        bounds.nodeIndex = entry.nodeIndex;
        bounds.ownerCount = entry.ownerCount;

        for (const asset::Vertex &vertex : entry.mesh.vertices)
            bounds.box.expand(vertex.position);
        for (const asset::Vertex &vertex : entry.mesh.bindVertices)
            bounds.bindBox.expand(vertex.position);

        if (entry.nodeIndex < nodeWorld.size())
        {
            const math::Mat4 &m = nodeWorld[entry.nodeIndex];
            float scale[3] = {0.0f, 0.0f, 0.0f};
            for (int axis = 0; axis < 3; ++axis)
            {
                // 列主序：第 axis 列的 xyz 长度就是该轴的缩放（旋转不改变列长）。
                const math::Vec3 column{m.at(0, axis), m.at(1, axis), m.at(2, axis)};
                scale[axis] = math::length(column);
            }
            bounds.nodeScale = math::Vec3{scale[0], scale[1], scale[2]};
        }

        out.push_back(bounds);
    }

    return out;
}

} // namespace my3d::render
