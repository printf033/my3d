// my3d::asset —— 动画采样与蒙皮实现。
//
// 刻意零第三方依赖：本文件只 include core/math、asset 自己的头与标准库。
// core / backend_cpu / backend_ascii / platform_tty 的「不链任何第三方库」这条
// 硬约束，靠的就是这条边界。

#include "animation.hpp"

namespace my3d::asset
{

namespace
{

math::Vec3 lerpVec3(const math::Vec3 &a, const math::Vec3 &b, float t) noexcept
{
    return math::lerp(a, b, t);
}

math::Quat slerpQuat(const math::Quat &a, const math::Quat &b, float t) noexcept
{
    return math::slerp(a, b, t);
}

} // namespace

ChannelPose sampleChannel(const AnimationChannel &channel, float t)
{
    ChannelPose pose;

    if (channel.drivesPosition())
    {
        pose.hasPosition = true;
        pose.position = sampleKeyRange(channel.positions, t, math::Vec3{}, lerpVec3);
    }

    if (channel.drivesRotation())
    {
        pose.hasRotation = true;
        pose.rotation =
            sampleKeyRange(channel.rotations, t, math::Quat::identity(), slerpQuat);
    }

    if (channel.drivesScale())
    {
        pose.hasScale = true;
        pose.scale = sampleKeyRange(channel.scales, t, math::Vec3{1.0f, 1.0f, 1.0f}, lerpVec3);
    }

    return pose;
}

void computeJointMatrices(const MeshData &mesh, const std::vector<math::Mat4> &nodeWorld,
                          std::vector<math::Mat4> &outJointMatrices, size_t *outInvalidJoints)
{
    const size_t jointCount = mesh.jointNodes.size();
    outJointMatrices.assign(jointCount, math::Mat4::identity());

    size_t invalid = 0;
    for (size_t j = 0; j < jointCount; ++j)
    {
        const uint32_t node = mesh.jointNodes[j];
        if (node == kInvalidIndex || node >= nodeWorld.size())
        {
            ++invalid;   // 保持 identity：该骨骼不驱动任何顶点位移，而不是随机姿势
            continue;
        }
        // 先逆绑定（网格空间 → 骨骼绑定空间），再乘当前世界矩阵（绑定空间 → 当前）。
        outJointMatrices[j] = nodeWorld[node] * mesh.inverseBind[j];
    }

    if (outInvalidJoints)
        *outInvalidJoints = invalid;
}

void skinVertices(const MeshData &mesh, const std::vector<math::Mat4> &jointMatrices,
                  std::vector<Vertex> &out)
{
    const size_t count = mesh.bindVertices.size();
    out.resize(count);

    for (size_t v = 0; v < count; ++v)
    {
        const Vertex &src = mesh.bindVertices[v];
        Vertex &dst = out[v];

        // 默认整份复制：UV 与切线手性不参与变形，位置/法线在下面按需覆盖。
        // 全零权重的顶点就走这条默认路径 —— 原样留在绑定姿态。
        dst = src;

        const SkinAttributes &sa = mesh.skinAttributes[v];

        math::Mat4 skin{};
        float weightSum = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
            const float w = sa.weights[k];
            if (!(w > 0.0f))
                continue;
            const size_t slot = sa.joints[k];
            if (slot >= jointMatrices.size())
                continue;

            const math::Mat4 &jm = jointMatrices[slot];
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    skin.at(r, c) += w * jm.at(r, c);
            weightSum += w;
        }

        if (!(weightSum > 0.0f))
            continue;   // 未被任何骨骼覆盖：保留绑定顶点

        // 权重归一化。导入时 aiProcess_LimitBoneWeights 把每顶点限制到 4 根骨，
        // 被丢弃的那部分权重不会自动补回来 —— 不除的话绑定姿态下顶点就已经按
        // Σw 缩放过一次，之后每一帧都带着这份系统性收缩（在 mia 这类模型上
        // 实测约 3e-5，量级小但恒定，不是舍入噪声）。除完再变换，绑定姿态下
        // skinning self-check 才会回到浮点噪声量级。
        const float invWeight = 1.0f / weightSum;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                skin.at(r, c) *= invWeight;

        const math::Vec4 p = math::transformPoint(skin, src.position);
        dst.position = math::Vec3{p.x, p.y, p.z};

        // 法线/切线只用蒙皮矩阵的线性部分，与顶点共享同一组权重。
        // 严格做法是逐骨骼求逆转置（只在非均匀缩放骨骼下与这里不同），代价是每顶点
        // 4 次 3x3 求逆 —— 在 ASCII 分辨率下不可见。这条注释是防止后来者以为漏了。
        const math::Vec3 n = math::transformVector(skin, src.normal);
        if (math::lengthSq(n) > 1e-20f)
            dst.normal = math::normalize(n);

        const math::Vec3 tg = math::transformVector(skin, math::Vec3{src.tangent.x, src.tangent.y,
                                                                     src.tangent.z});
        if (math::lengthSq(tg) > 1e-20f)
        {
            const math::Vec3 t3 = math::normalize(tg);
            dst.tangent = math::Vec4{t3.x, t3.y, t3.z, src.tangent.w};
        }
    }
}

} // namespace my3d::asset
