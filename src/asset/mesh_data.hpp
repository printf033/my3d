#pragma once

// mesh 的引擎侧表示：扁平顶点/索引数组 + 局部包围盒。
// 不包含任何后端句柄 —— GPU 资源由 IRenderDevice 按需创建。

#include "core/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::asset
{

// interleaved 顶点，可直接上传 GPU。
//
// 与旧 Vertex 的差异：旧代码用 filament::math::quatf 存 packTangentFrame 的
// 四元数 —— 那是 Filament 的存储编码，不是引擎概念。这里用 glTF 的标准表达：
// normal + tangent(vec4)，其中 tangent.w 是手性 ±1，副切线由 cross(n,t)*w 得到。
// 后端需要什么编码由后端自己转换。
struct Vertex
{
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec4 tangent;   // xyz = 切线，w = 手性 (±1)
    math::Vec2 uv0;
};

static_assert(sizeof(Vertex) == 3 * 4 + 3 * 4 + 4 * 4 + 2 * 4, "Vertex must stay tightly packed");

// 顶点蒙皮属性：4 个骨骼槽位 + 4 个权重，与 vertices 同长同序。
//
// 为什么单独一个数组，而不是并进 Vertex：Vertex 的 48 字节是后端的硬契约
// （Filament 的 GpuVertex 按 48 字节 stride 上传，且注释写明选 48 而非紧凑的 44
// 是为了让 TANGENTS 落在 16 字节边界）。为了一个只有 CPU 蒙皮路径需要的属性去
// 改所有后端的顶点布局，代价和收益不成比例。顶点布局不动，蒙皮额外挂一条。
//
// 槽位是 uint16_t：骨骼数上限 65535，对任何实际资产都够，而 uint16 让整个结构
// 停在 24 字节（uint16_t[4] + float[4]）。joints 里存的是**骨骼槽位**，不是节点
// 索引 —— 槽位经 MeshData::jointNodes 才映射到 AssetNode。
struct SkinAttributes
{
    uint16_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(SkinAttributes) == 4 * 2 + 4 * 4, "SkinAttributes must stay tightly packed");

struct MeshData
{
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    math::AABB bounds;   // 模型空间，来自 aiProcess_GenBoundingBoxes

    // ---------------- 蒙皮（可选） ----------------
    //
    // 非蒙皮网格这四项全空，所有既有代码路径原样不受影响。
    //
    // vertices 与 bindVertices 的分工是这一层的核心约定：bindVertices 是导入时的
    // 原始数据、此后不再改；vertices 是「当前用于绘制的」。蒙皮每帧**从 bindVertices
    // 重算** vertices，而不是在上一帧的 vertices 上接着变形 —— 后者会让浮点误差
    // 逐帧累积（几十秒后模型开始发飘），而重算天然幂等，也天然支持回落到绑定姿态。
    std::vector<Vertex> bindVertices;             // 非空 = 蒙皮网格
    std::vector<SkinAttributes> skinAttributes;   // 与 vertices 同长
    std::vector<uint32_t> jointNodes;             // 骨骼槽位 → AssetNode 索引
    std::vector<std::string> jointNames;          // 与 jointNodes 同序，诊断用
    std::vector<math::Mat4> inverseBind;          // 与 jointNodes 同序，骨骼空间 → 网格空间

    // 三项齐全才算可蒙皮：蒙皮要读所有权重、要把槽位映射到节点、要逆绑定矩阵。
    // 缺任何一项都只能跳过，而不是拿残缺数据算出一具扭曲的模型。
    bool skinned() const noexcept
    {
        return !bindVertices.empty() && !skinAttributes.empty() && !jointNodes.empty() &&
               bindVertices.size() == vertices.size() &&
               skinAttributes.size() == vertices.size() &&
               inverseBind.size() == jointNodes.size() &&
               jointNames.size() == jointNodes.size();
    }

    uint32_t vertexCount() const noexcept { return static_cast<uint32_t>(vertices.size()); }
    uint32_t indexCount() const noexcept { return static_cast<uint32_t>(indices.size()); }
    uint32_t triangleCount() const noexcept { return indexCount() / 3u; }
};

} // namespace my3d::asset
