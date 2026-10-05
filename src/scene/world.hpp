#pragma once

// my3d::scene —— 后端无关的场景描述。
//
// World 是 IRenderer::beginFrame 的主语：Filament 后端把它翻译成自己的
// Scene/Entity；CPU/ASCII 后端遍历它、剔除、排序、自己栅格化。
//
// 「接口以场景为准，不以绘制调用为准」是这套架构最关键的单个决策：
// Filament 需要完整场景才能做剔除 / 阴影 / IBL，若接口只给一串 draw call，
// Filament 后端就只能绕过接口自己重建场景图，抽象即告失效。

#include "core/handle.hpp"
#include "core/math.hpp"
#include "light.hpp"
#include "transform.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::scene
{

// 一个绘制片元：网格 + 材质，可指向网格内的索引子区间。
struct Primitive
{
    MeshHandle mesh;
    MaterialHandle material;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;   // 0 表示「使用整个网格」
};

struct Node
{
    std::string name;
    Transform transform;

    math::AABB meshBounds;     // 本节点自身网格的并集（节点空间）
    math::AABB localBounds;    // 含整棵子树（节点空间），updateTransforms() 填充
    math::AABB worldBounds;    // 同上，但已变换到世界空间，剔除直接用这个

    std::vector<Primitive> primitives;
    std::vector<uint32_t> children;
    uint32_t parent = kInvalidIndex;

    math::Mat4 worldMatrix = math::Mat4::identity();
    bool visible = true;
};

// 环境光照。刻意不持有 asset 类型：asset 层加载出的球谐在 app 层转成裸数组，
// 于是 scene 不需要知道 IBL 的文件格式。
struct Environment
{
    IblHandle ibl;                                     // 后端 IBL 资源；无效 = 无
    float iblIntensity = 1.0f;
    math::Vec3 ambientColor{0.02f, 0.02f, 0.02f};      // 无 IBL 时的常量环境项
    math::Vec3 reflectionColor{1.0f, 1.0f, 1.0f};
    math::Vec3 shBands[9]{};                           // L0..L8 的 RGB；全零 = 未设置
};

class World
{
public:
    // ---------- 层级 ----------
    uint32_t createNode(const std::string &name = {});
    Node *node(uint32_t index) noexcept;
    const Node *node(uint32_t index) const noexcept;
    size_t nodeCount() const noexcept { return nodes_.size(); }
    bool empty() const noexcept { return nodes_.empty(); }

    // 设为 parent 的子节点；parent 传 kInvalidIndex 表示成为根。
    // 返回 false 表示索引非法，或该操作会形成父子环。
    bool setParent(uint32_t child, uint32_t parent);
    uint32_t parentOf(uint32_t index) const noexcept;

    void attachPrimitive(uint32_t nodeIndex, const Primitive &primitive);

    // ---------- 光照 ----------
    void addLight(const LightDesc &light) { lights_.push_back(light); }
    std::vector<LightDesc> &lights() noexcept { return lights_; }
    const std::vector<LightDesc> &lights() const noexcept { return lights_; }

    // ---------- 环境 ----------
    Environment &environment() noexcept { return environment_; }
    const Environment &environment() const noexcept { return environment_; }

    // ---------- 求值 ----------
    // 重算 worldMatrix / worldBounds。返回 false 表示层级数据损坏（环或越界）。
    bool updateTransforms();
    const std::vector<math::Mat4> &worldMatrices() const noexcept { return world_; }
    math::AABB worldBounds() const noexcept;

    std::vector<Node> &nodes() noexcept { return nodes_; }
    const std::vector<Node> &nodes() const noexcept { return nodes_; }

    void clear();

private:
    std::vector<Node> nodes_;
    std::vector<math::Mat4> world_;
    std::vector<LightDesc> lights_;
    Environment environment_;
};

} // namespace my3d::scene
