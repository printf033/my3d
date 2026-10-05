#pragma once

// my3d::backend::filament —— GPU 后端（Filament）。
//
// 与其他后端的关系：和 backend/cpu、backend/ascii 平级，都是 IRenderer /
// IRenderDevice 的实现。三者的差别只在「谁来画」：
//   * cpu/ascii —— 自己栅格化，所有资源是自己的内存；
//   * filament  —— 把场景翻译成 Filament 的 Entity/Scene，交给 Filament 画。
//
// ============================ 翻译的两个方向 ============================
//
// 本类做的是**单向翻译**：引擎侧的 World / MeshData / MaterialDesc 翻译成
// Filament 对象，Filament 的任何类型都不出现在公开接口上（除了窗口句柄，
// 那本来就是个 void*）。上层代码因此可以在不知道 Filament 存在的前提下被编译。
//
// ============================ 与旧 engine.hpp 的差异 ============================
//
// 旧实现是一个 681 行的单体 Engine，用 std::unordered_map<std::string, T*> 做
// 资源表，并且以 ~Engine() = default 结束 —— **所有 Filament 资源都不回收**。
// 那个析构是迁移时必须修掉的缺陷，不是可以照抄的既有设计。本类的析构按
// 依赖倒序显式销毁：Renderable → 材质实例 → 材质 → 纹理 → 网格 → 灯光 →
// 环境 → 相机 → View → Scene → Renderer → SwapChain → RenderTarget → Engine。
//
// ============================ 单位与约定 ============================
//   * 顶点绕序：CCW 为正面（Filament 默认）。importer 已保证，见 createMesh。
//   * 矩阵：列主序，与 core/math.hpp 的 Mat4 一致，与 filament::math::mat4f 一致。
//   * 色彩：BaseColor / Emissive 纹理以 sRGB 内部格式上传，其余为线性。
//     搞混会让光照方向整体偏移 —— 症状像法线错了，实际只是 gamma 解了两次。

#include "asset/image_data.hpp"
#include "asset/material.hpp"
#include "asset/mesh_data.hpp"
#include "core/handle.hpp"
#include "render/renderer.hpp"
#include "render/types.hpp"
#include "scene/world.hpp"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderTarget.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace my3d::backend::filament
{

using namespace ::filament;   // NOLINT —— 后端实现文件里消歧义代价高于收益

// 三套着色模型到已编译材质包的映射。
// 路径相对 assetRoot，产物由 src/mat/*.mat 经 matc 编译而来。
struct MaterialPackPaths
{
    std::string unlit = "assets/shader/desktop/vulkan/unlit.filamat";
    std::string lit = "assets/shader/desktop/vulkan/lit.filamat";
    std::string litAorm = "assets/shader/desktop/vulkan/lit_aorm.filamat";
};

class FilamentRenderer final : public render::IRenderer, public render::IRenderDevice
{
public:
    // assetRoot 是 filamat / ktx 的相对根目录（通常是仓库根或可执行文件旁的
    // 资产目录）。构造会创建 Filament Engine —— 失败抛 std::runtime_error。
    explicit FilamentRenderer(std::string assetRoot = ".",
                              MaterialPackPaths packs = {},
                              Engine::Backend backend = Engine::Backend::DEFAULT);
    ~FilamentRenderer() override;

    FilamentRenderer(const FilamentRenderer &) = delete;
    FilamentRenderer &operator=(const FilamentRenderer &) = delete;
    FilamentRenderer(FilamentRenderer &&) = delete;
    FilamentRenderer &operator=(FilamentRenderer &&) = delete;

    // ---------------- 窗口 / 离屏绑定 ----------------
    //
    // 必须在 beginFrame 之前调用一次。两条路互斥：
    //   attachSwapChain —— 有原生窗口，渲染直接上屏（present 由 Filament 做）；
    //   attachOffscreen —— 无窗口，渲到 RenderTarget，用 readbackImage 取回。
    //
    // 分开是因为 headless swapchain 读不出像素（Filament 的 headless
    // SwapChain 没有 surface，readPixels 恒返回全黑），离屏路径必须走
    // RenderTarget —— 这点是实测出来的，不是设计偏好。
    bool attachSwapChain(void *nativeWindow, uint64_t flags = 0);
    void detachSwapChain();
    bool attachOffscreen(uint32_t width, uint32_t height);

    bool hasSwapChain() const noexcept { return swapChain_ != nullptr; }

    // ---------------- IRenderer ----------------
    bool beginFrame(const render::FrameInfo &info) override;
    void endFrame() override;
    bool resize(uint32_t width, uint32_t height) override;
    const char *name() const noexcept override { return "filament"; }
    bool readbackImage(asset::ImageData &out) override;

    // ---------------- IRenderDevice ----------------
    MeshHandle createMesh(const asset::MeshData &mesh) override;
    TextureHandle createTexture(const asset::ImageData &image, render::TextureUsage usage) override;
    MaterialHandle createMaterial(const asset::MaterialDesc &material) override;
    bool updateMeshVertices(MeshHandle mesh, const std::vector<asset::Vertex> &vertices) override;

    void destroy(MeshHandle mesh) override;
    void destroy(TextureHandle texture) override;
    void destroy(MaterialHandle material) override;

    // ---------------- Filament 特有扩展 ----------------
    //
    // 这些**不属于** IRenderDevice：IRender 接口的主体是 asset 层的通用类型，
    // IBL / 天空盒只对 GPU 后端有意义（CPU 光栅器用 shBands 走另一条路），
    // 塞进接口就是「通用接口被某一种后端的私货污染」。
    //
    // loadIblFromKtx：按扩展名分派 Ktx1(.ktx) / Ktx2(.ktx2) 读取器。
    //   srgb=false 用于 IBL（辐照度是线性光），srgb=true 用于天空盒。
    IblHandle loadIblFromKtx(const std::string &relativePath);
    bool setSkyboxFromKtx(const std::string &relativePath);
    void destroy(IblHandle ibl);

    // SH 有 9 个系数、每系数一个 float3，与 scene::Environment::shBands 同构。
    bool applyEnvironment(const scene::Environment &env);

    // 调试 / 统计
    size_t meshCount() const noexcept { return meshes_.size(); }
    size_t textureCount() const noexcept { return textures_.size(); }
    size_t materialCount() const noexcept { return materials_.size(); }
    uint32_t lastFramePrimitives() const noexcept { return lastFramePrimitives_; }
    Engine &engine() noexcept { return *engine_; }

private:
    // ---------------- GPU 侧资源记录 ----------------
    //
    // 句柄由 SlotMap 管理（与 cpu 后端同一套），generation 校验能挡住悬垂句柄。
    // 记录里存 Filament 裸指针：生命周期由本类独占，Engine 销毁时一并释放。

    struct MeshRecord
    {
        VertexBuffer *vb = nullptr;
        IndexBuffer *ib = nullptr;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        math::AABB bounds;
        bool skinned = false;
    };

    struct TextureRecord
    {
        Texture *texture = nullptr;
        render::TextureUsage usage = render::TextureUsage::BaseColor;
        bool ownsTexture = true;   // false 表示是从 IBL 借来的（不在这里销毁）
    };

    struct MaterialRecord
    {
        Material *material = nullptr;              // 来自已加载的包，不单独销毁
        MaterialInstance *instance = nullptr;      // 由 createMaterial 创建，要销毁
        asset::ShadingModel model = asset::ShadingModel::Lit;
        bool doubleSided = false;
    };

    // IBL 记录。reflections 是一张 cubemap 纹理，通常由 KTX 读出；
    // loadIblFromKtx 每次调用都会新建一张，所以在表里记所有权。
    struct IblRecord
    {
        IndirectLight *indirectLight = nullptr;
        Texture *reflections = nullptr;
        bool ownsReflections = true;
    };

    // 每个 (node, primitive) 对应一个 Filament Entity。key 打包成 uint64。
    struct PrimitiveRecord
    {
        utils::Entity entity{};
        MeshHandle mesh;
        MaterialHandle material;
        uint32_t indexOffset = 0;   // 与 world 侧 Primitive 同步；变化时重建实体
        uint32_t indexCount = 0;
        bool hasTransformComponent = false;
        math::Mat4 lastMatrix = math::Mat4::identity();
        uint32_t lastSeenFrame = 0;
    };

    // ---------------- 内部步骤 ----------------
    bool loadMaterialPacks();
    bool loadMaterialPack(const std::string &relativePath, Material *&out);

    // 把 MeshData 转成 Filament 的顶点/索引缓冲。
    //
    // **这是本后端最容易出错的单个函数**，因为 Filament 的 TANGENTS 属性不是
    // 「切线向量」而是「打包成四元数的整个 TBN 正交基」，且没有独立的 NORMAL
    // 属性。引擎侧 Vertex 存的是 glTF 标准表达 (tangent.xyz, tangent.w=手性)，
    // 必须在这里展开成 (t, b, n) 再 packTangentFrame()。详见 .cpp。
    bool buildVertexBuffer(const asset::MeshData &mesh, VertexBuffer *&vb, IndexBuffer *&ib);

    // 纹理按 source 去重：同一张图被多个材质引用时只上传一次。
    Texture *acquireTexture(const asset::TextureSlot &slot, render::TextureUsage usage);
    Texture *uploadImage(const asset::ImageData &image, render::TextureUsage usage);

    // 1x1 中性纹理（按用途缓存）。
    //
    // 存在的理由是 Filament 的硬约束：材质里声明过的每个 sampler 都必须被
    // setParameter 设上，否则运行时警告 + 着色器读到垃圾。而引擎侧的
    // MaterialDesc 允许「只给 factor 不给贴图」—— 这时 factor 与纹理是逐分量
    // 相乘的，缺纹理必须用**身份元**补位，否则 factor 会被乘成 0。
    // 具体填充值见 .cpp 的 fillNeutralTexel()：这里有一步 emissive 填白（不是
    // 填黑）、normal 填 (128,128,255) 的讲究，写错不报错但效果全错。
    Texture *neutralTexture(render::TextureUsage usage);

    // 把 MaterialDesc 里分开的 ambientOcclusion / roughness / metallic 三张图
    // 合成单张 aorm（R=AO, G=Roughness, B=Metallic），供 LitAORM 模型使用。
    // 三张都缺时返回空指针，调用方回落到中性纹理。
    asset::ImagePtr packAormImage(const asset::MaterialDesc &desc);

    // 销毁实体（先 Scene::remove 再 engine->destroy）。所有实体销毁的唯一出口。
    void destroyEntity(utils::Entity entity);

    // 同步 World → Filament Scene：增删改实体、更新变换、同步灯光与环境。
    void syncWorld(const scene::World &world);
    void syncLights(const scene::World &world);
    void updateCamera(const render::View &view);
    void applyRenderSettings(const render::RenderSettings &settings);
    std::string resolvePath(const std::string &relativePath) const;

    // ---------------- Filament 核心对象 ----------------
    Engine *engine_ = nullptr;
    Renderer *renderer_ = nullptr;
    Scene *scene_ = nullptr;
    View *view_ = nullptr;
    Camera *camera_ = nullptr;
    utils::Entity cameraEntity_{};

    SwapChain *swapChain_ = nullptr;
    RenderTarget *offscreenRt_ = nullptr;
    Texture *offscreenColor_ = nullptr;
    Texture *offscreenDepth_ = nullptr;

    // ---------------- 已加载的材质包 ----------------
    Material *unlitMaterial_ = nullptr;
    Material *litMaterial_ = nullptr;
    Material *litAormMaterial_ = nullptr;

    // ---------------- 环境 ----------------
    IndirectLight *indirectLight_ = nullptr;
    Skybox *skybox_ = nullptr;
    // 天空盒用的环境贴图。Skybox 对象**不持有**它，所有权在这里 —— 换天空盒
    // 或析构时都要跟着销毁。漏了就只是显存泄漏（不崩、不报错），属于那种
    // 要跑很久、显存慢慢涨才看得出来的缺陷。
    Texture *skyboxEnvironment_ = nullptr;

    // ---------------- 资源表 ----------------
    SlotMap<MeshRecord, MeshTag> meshes_;
    SlotMap<TextureRecord, TextureTag> textures_;
    SlotMap<MaterialRecord, MaterialTag> materials_;
    SlotMap<IblRecord, IblTag> ibls_;

    // 纹理去重表：asset 层的 source 字符串 / embedded 键 → 已上传的纹理。
    std::unordered_map<std::string, Texture *> textureBySource_;

    // 中性纹理缓存。不进 SlotMap —— 它们的生命周期与本对象绑定，
    // 且句柄对外不可见（调用方无从 destroy 它们）。
    std::unordered_map<render::TextureUsage, Texture *> neutralTextures_;

    // 绑定了离屏 RenderTarget。readbackImage 只在这条路径下可用。
    bool offscreenBound_ = false;

    // ---------------- 世界翻译缓存 ----------------
    std::unordered_map<uint64_t, PrimitiveRecord> primitives_;
    // 灯光实体按 world 中 lights() 的顺序逐个同步；数量变化时增删。
    std::vector<utils::Entity> lightEntities_;
    uint32_t frameCounter_ = 0;
    uint32_t lastFramePrimitives_ = 0;

    // applyRenderSettings 的告警节流：Filament 的 View 没有线框 / 深度测试
    // 开关，只在第一次遇到时提醒一次，避免每帧刷屏。
    bool warnedWireframe_ = false;
    bool warnedDepthTest_ = false;
    // 当前 RenderSettings::showEnvironment 的值。setSkyboxFromKtx 在还没有
    // 传过 View 的情况下也要知道「新天空盒该不该立刻绑到 Scene 上」——
    // 否则 headless 预加载天空盒会静默不生效，直到第一帧才被纠正。
    bool showEnvironment_ = true;

    // ---------------- 环境 ----------------
    // 已应用到 IndirectLight 上的球谐系数。Engine 侧的 Environment 每帧都
    // 会重新传进来，但 IndirectLight 的 SH 只能通过 Builder 设置（没有
    // setter），重建代价高 —— 所以缓存一份用于比对，变了才重建。
    math::Vec3 appliedShBands_[9]{};
    bool hasAppliedSh_ = false;
    // 当前场景绑定的 IBL；destroy(IblHandle) 用它判断是否需要解绑。
    IblHandle currentIbl_;

    // ---------------- 配置 ----------------
    std::string assetRoot_;
    MaterialPackPaths packs_;
    uint32_t targetWidth_ = 1280;
    uint32_t targetHeight_ = 720;
};

} // namespace my3d::backend::filament
