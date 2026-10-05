#pragma once

// my3d::render —— 引擎级渲染抽象（红线所在）。
//
// ============================ 这不是 RHI ============================
//
// 接口停留在**引擎概念层**：场景、网格、材质、纹理、视图、帧。
// 接口里**不允许**出现：createBuffer / createPipeline / createDescriptorSet /
// createCommandBuffer / cmdDrawIndexed / 显存布局 / 描述符绑定 / 屏障 / 队列。
//
// 判据很简单：如果某个概念只有「用 Vulkan 的人」才需要知道，它就不该出现在
// 这里。Vulkan 是 backend/vulkan/ 的内部实现细节，与 Filament 处于同一层级，
// 二者都只是 IRenderer / IRenderDevice 的实现。
//
// 为什么要有 IRenderDevice 这一层显式资源生命周期：Filament 的资源由 Engine
// 持有、CPU 后端由自己的 arena 持有；把 create/destroy 放进接口，是为了让
// 「在 ASCII 后端上跑同一份场景代码」不需要 #ifdef。

#include "asset/image_data.hpp"
#include "asset/material.hpp"
#include "asset/mesh_data.hpp"
#include "core/handle.hpp"
#include "render/types.hpp"

namespace my3d::render
{

// 纹理用途决定采样色彩空间与后端内部布局，与具体格式无关。
enum class TextureUsage : uint32_t
{
    BaseColor,           // sRGB
    Normal,
    MetallicRoughness,   // 线性
    Emissive,            // sRGB
    AmbientOcclusion     // 线性
};

class IRenderer
{
public:
    virtual ~IRenderer() = default;

    // 后端自行完成：剔除 / 排序 / 栅格化 / present。调用方不介入。
    virtual bool beginFrame(const FrameInfo &info) = 0;
    virtual void endFrame() = 0;

    virtual bool resize(uint32_t width, uint32_t height) = 0;

    virtual const char *name() const noexcept = 0;

    // 离屏读回，供 golden image 回归与 ASCII 调试。
    // GPU 后端返回 false 是合法实现（读回需要同步等待，不应成为通用路径）。
    virtual bool readbackImage(asset::ImageData &out)
    {
        (void)out;
        return false;
    }
};

class IRenderDevice
{
public:
    virtual ~IRenderDevice() = default;

    virtual MeshHandle createMesh(const asset::MeshData &mesh) = 0;
    virtual TextureHandle createTexture(const asset::ImageData &image, TextureUsage usage) = 0;
    virtual MaterialHandle createMaterial(const asset::MaterialDesc &material) = 0;

    virtual void destroy(MeshHandle mesh) = 0;
    virtual void destroy(TextureHandle texture) = 0;
    virtual void destroy(MaterialHandle material) = 0;

    // 原地替换一个网格的顶点数据，索引与拓扑不变。
    //
    // 这是骨骼蒙皮每帧唯一需要的能力：骨骼只改顶点位置，不增删三角形，所以既不该
    // 走「销毁 + 重建」（每帧丢一次资源），也不该为此在接口里暴露 buffer 概念。
    //
    // 默认实现返回 false = 「本后端不支持原地更新」，而不是静默假装成功。调用方
    // 据此能区分「动画没做对」与「动画算对了但没送到后端」，后者是静默失败最典型的
    // 藏身处 —— 屏幕上两者都只是一帧不动的画面。
    //
    // 约定：vertices.size() 必须等于建资源时的顶点数；不等时后端应返回 false（布局
    // 变了就不是「原地更新」，需要重建资源）。
    virtual bool updateMeshVertices(MeshHandle mesh, const std::vector<asset::Vertex> &vertices)
    {
        (void)mesh;
        (void)vertices;
        return false;
    }
};

} // namespace my3d::render
