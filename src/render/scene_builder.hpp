#pragma once

// my3d::render —— 资产场景 → 运行时场景的装配。
//
// 这一段是两半之间的缝：asset::AssetScene 是导入器的输出（纯数据、无句柄），
// scene::World 是 IRenderer::beginFrame 的主语（含后端句柄）。如果没有一个
// 明确的落点，这段胶水就会长在每个 sample 里 —— 于是「换后端不改场景代码」
// 在样例层就先失效了，而这正是这套 API 存在的理由（docs/architecture.md §0）。
//
// 放在 render/ 而不是 sample 里，是因为它恰好只依赖三样东西，且都在 render 层
// 的职责范围内：
//
//   asset（数据） + IRenderDevice（句柄工厂） + scene::World（场景图）
//
// 它**不**知道窗口、不知道终端、不知道 Filament —— include 列表就是证据。
// 后端差异全部由传进来的 device 吃掉。
//
// 不变量：buildWorldFromAsset 是**覆盖**语义（先清空 world，再重填），因为
// 「场景 = 一次装配的结果」比「往旧场景里叠加」更少歧义。需要多场景合并时，
// 由调用方多次调用并自己管理节点索引偏移，不在这里猜。

#include "asset/asset_scene.hpp"
#include "render/renderer.hpp"
#include "scene/world.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::render
{

struct SceneBuildStats
{
    uint32_t nodes = 0;
    uint32_t primitives = 0;
    uint32_t meshes = 0;
    uint32_t materials = 0;

    // 被跳过的 primitive 数（索引越界，见 .cpp 的说明）。正常的资产应当为 0；
    // 不为 0 时画面会缺块，所以它单列出来，而不是混进 primitives。
    uint32_t skippedPrimitives = 0;
};

struct SceneBuildResult
{
    bool ok = false;
    std::string error;                  // ok == false 时非空
    std::vector<std::string> warnings;  // ok == true 时也可能非空
    SceneBuildStats stats;

    // 建出来的资源句柄，下标与 AssetScene::meshes / materials 的下标一一对应。
    //
    // 这份映射必须由装配层交出来：蒙皮驱动要按 mesh 索引找到「该更新哪个后端
    // 资源」，而句柄是 SlotMap 发的，调用方无处可推。让调用方去猜「句柄 = 下标 +
    // 固定 generation」等于把 SlotMap 的内部布局变成公开契约 —— 一旦空闲列表复用
    // 过槽位，猜出来的句柄就指向别的网格。
    std::vector<MeshHandle> meshHandles;
    std::vector<MaterialHandle> materialHandles;
};

// 把 assetScene 装进 world：
//
//   1. 每个 MeshData / MaterialDesc 经 device 建一个后端资源；
//   2. 每个 AssetNode 建一个 scene::Node，局部变换走 Transform::matrixOverride
//      直通（见 scene/transform.hpp 的理由）；
//   3. 连父子、挂 primitive、转写 meshBounds；
//   4. 求值一次世界变换（world.updateTransforms()）。
//
// 关于 meshBounds：attachPrimitive 不替调用方填包围盒，因为同一个网格被不同
// 节点缩放时包围盒并不相同 —— 这是调用方的契约。这里是最有资格履行它的那个
// 调用方：AssetNode::meshBounds 是导入器在**节点空间**算好的，正是这个字段要
// 的语义。（漏填不报错，只让视锥剔除对这些节点永久失效。）
//
// 关于纹理：本函数**不**调 createTexture，原因有两条，都不是遗漏 ——
//
//   * MaterialDesc 携带的是 ImagePtr（数据），不是句柄，CPU / ASCII 在绘制时直接
//     从材质槽采样；创建独立 TextureHandle 对它们没有收益；
//   * GPU 后端在 createMaterial 内把图像上传为自己的纹理并持有句柄，不必改此接口。
//
// 失败（无节点、层级成环、父索引越界）返回 ok = false；此时 world 内容不保证
// 可用，调用方应当丢弃它而不是渲染。
SceneBuildResult buildWorldFromAsset(const asset::AssetScene &assetScene,
                                     IRenderDevice &device,
                                     scene::World &world);

} // namespace my3d::render
