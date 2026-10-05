// my3d::backend::filament —— GPU 后端实现。
//
// 阅读顺序建议：构造 → loadMaterialPacks → createMesh / createTexture /
// createMaterial → syncWorld → beginFrame。其余是生命周期与 KTX 辅助。

#include "backend/filament/filament_renderer.hpp"

#include <filament/Box.h>
#include <filament/Color.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>

#include <image/Ktx1Bundle.h>
#include <ktxreader/Ktx1Reader.h>
#include <ktxreader/Ktx2Reader.h>

#include <math/mat3.h>
#include <math/quat.h>

#include <utils/EntityManager.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace my3d::backend::filament
{

// =====================================================================
// 局部辅助
// =====================================================================
namespace
{

// ---------------------------------------------------------------------
// Filament 异步上传的所有权约定
//
// BufferDescriptor / PixelBufferDescriptor 持有的是**裸指针**，上传却是异步
// 的：Filament 要到渲染线程执行到那条命令时才 memcpy 读它。所以「临时缓冲的
// 地址 + 不给 callback」必然 UAF —— 栈数组或局部 vector 在 setBufferAt /
// setImage 返回时就已经析构，渲染线程读到的是已释放的内存。
//
// 这类错误不会稳定复现，而且会把人指向完全错误的方向：内存还没被复用时内容
// 仍是旧的顶点数据，画面看起来完全正常；只有在堆布局恰好被别的分配改变时才
// 显形，表现为「模型整片消失」或「整屏纯白」，与 IBL、光照、清屏色都没有
// 因果。MALLOC_PERTURB_ 与 ASan 是让它现形的可行手段（见 docs/architecture.md）。
//
// 所以本文件送给 Filament 的每一块 CPU 缓冲都走下面两条路：拷到堆，并把释放
// 权经 callback 交给 Filament —— descriptor 在数据被消费后、或自身析构时调用
// 它（backend/BufferDescriptor.h:44 的保证）。
// ---------------------------------------------------------------------

// 释放回调的签名必须匹配 BufferDescriptor::Callback = void(*)(void*, size_t, void*)。
void freeOwnedBuffer(void *buffer, size_t /*size*/, void * /*user*/) noexcept
{
    std::free(buffer);
}

// 拷贝一份到堆，所有权随即交给 Filament。失败返回 nullptr。
// 调用方在失败时必须销毁已经建好的 Filament 对象，让 descriptor 的回调不被注册。
void *copyToOwnedHeap(const void *src, size_t bytes)
{
    if (bytes == 0)
        return nullptr;
    void *dst = std::malloc(bytes);
    if (dst)
        std::memcpy(dst, src, bytes);
    return dst;
}

// 从零向量退化的法线/切线造一组正交基。
// 旧 engine.hpp 的写法，行为正确：选一个与法线不平行的参考轴再叉乘。
inline math::Vec3 orthogonalFallback(const math::Vec3 &n) noexcept
{
    const math::Vec3 up = std::abs(n.z) < 0.999f ? math::Vec3{0.0f, 0.0f, 1.0f}
                                                 : math::Vec3{1.0f, 0.0f, 0.0f};
    return math::normalize(math::cross(up, n));
}

struct GpuVertex
{
    float px, py, pz, pad0;
    float tq[4];
    float u, v;
    float pad1[2];
};
static_assert(sizeof(GpuVertex) == 48, "GpuVertex layout must stay 48 bytes");

inline Box toFilamentBox(const math::AABB &aabb) noexcept;

void packGpuVertex(const asset::Vertex &src, GpuVertex &dst)
{
    math::Vec3 n = src.normal;
    const float nLen = math::length(n);
    n = nLen > 1e-8f ? n / nLen : math::Vec3{0.0f, 0.0f, 1.0f};

    math::Vec3 t{src.tangent.x, src.tangent.y, src.tangent.z};
    const float handedness = src.tangent.w < 0.0f ? -1.0f : 1.0f;
    t = t - n * math::dot(n, t);
    const float tLen = math::length(t);
    t = tLen > 1e-8f ? t / tLen : orthogonalFallback(n);

    const math::Vec3 b = math::cross(n, t) * handedness;

    filament::math::mat3f basis;
    basis[0] = filament::math::float3{t.x, t.y, t.z};
    basis[1] = filament::math::float3{b.x, b.y, b.z};
    basis[2] = filament::math::float3{n.x, n.y, n.z};

    const filament::math::quatf q = filament::math::mat3f::packTangentFrame(basis, sizeof(float));
    dst.px = src.position.x;
    dst.py = src.position.y;
    dst.pz = src.position.z;
    dst.pad0 = 0.0f;
    dst.tq[0] = q.x;
    dst.tq[1] = q.y;
    dst.tq[2] = q.z;
    dst.tq[3] = q.w;
    dst.u = src.uv0.x;
    dst.v = src.uv0.y;
    dst.pad1[0] = 0.0f;
    dst.pad1[1] = 0.0f;
}

inline bool readFileBytes(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(out.data()), size);
    return in.good() || in.eof();
}

// 文件名是否以 suffix 结尾（用于 .ktx / .ktx2 分派）。
inline bool endsWith(const std::string &s, const char *suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Ktx2Reader 只转码到被**显式请求过**的格式，没请求的一律返回 nullptr。
//
// 实测结论（本仓库 libs/filament 这份构建）：requestFormat 只接受 SRGB8_A8 与
// RGBA8，其余一律返回 FORMAT_UNSUPPORTED —— 打印诊断实测为
//   RGB16F=3 SRGB8_A8=0 SRGB8=3 RGBA8=0 RGB8=3   （0=SUCCESS, 3=FORMAT_UNSUPPORTED）
// 后三个请求因此是空转，留着只为上游将来放开格式时自动生效，不改变当前结果。
//
// 注意：**格式从来不是 .ktx2 IBL 加载失败的原因**，别再从这条线索查。真正的原因
// 见 loadKtx2Texture 顶部关于 cubemap 的说明。
inline void registryFormatsForKtx2(ktxreader::Ktx2Reader &reader)
{
    // 顺序即优先级：先 HDR 浮点，再 8-bit。
    //
    // 两类 transfer function 都各要一份 —— Ktx2Reader::load 的 transfer 参数
    // 不仅是「与文件元数据对碰」（对不上就失败），还**作为过滤条件决定最终
    // 内部格式**（见 Ktx2Reader.h:86 "used as a filter"）。只请求非 sRGB 格式的
    // 话，sRGB 源永远找不到匹配，load 只能返回 nullptr。
    reader.requestFormat(Texture::InternalFormat::RGB16F);
    reader.requestFormat(Texture::InternalFormat::SRGB8_A8);
    reader.requestFormat(Texture::InternalFormat::SRGB8);
    reader.requestFormat(Texture::InternalFormat::RGBA8);
    reader.requestFormat(Texture::InternalFormat::RGB8);
}

// KTX1 / KTX2 的文件标识符（KTX 规范原文：identifier 字段）。
// 用途只有一个：在把字节交给解码器**之前**确认它真的是这个格式。
// Ktx1Bundle 的构造函数里带 Precondition，喂给它一段随机字节会抛
// utils::PreconditionPanic 并穿过整个程序 —— 一个「用户给了坏文件」不该
// 升级成进程 abort。先自己看一眼魔数，坏输入就能走正常失败路径。
constexpr uint8_t kKtx1Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
                                         0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr uint8_t kKtx2Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                         0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

inline bool hasIdentifier(const std::vector<uint8_t> &bytes, const uint8_t (&id)[12])
{
    return bytes.size() >= 12 && std::memcmp(bytes.data(), id, 12) == 0;
}

// KTX2 header 的 faceCount 字段（偏移 36，小端；见 KTX 2.0 规范）。
// 只用来在解码前识别「这是不是立方体贴图」，不解析其余布局。
inline uint32_t ktx2FaceCount(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < 40)
        return 0;
    return static_cast<uint32_t>(bytes[36]) | (static_cast<uint32_t>(bytes[37]) << 8) |
           (static_cast<uint32_t>(bytes[38]) << 16) | (static_cast<uint32_t>(bytes[39]) << 24);
}

// 从 KTX2 字节建一个 Texture。
//
// transfer function 这里不能靠调用方猜：Ktx2Reader 会拿它和文件 DFD 里自报的
// 标记**对碰**，不一致就打印
//   "Source texture is marked sRGB, but client is requesting linear."
// 并返回 nullptr —— 这是硬失败，不是可以忽略的警告。仓库里 toktx 生成的
// ibl.ktx2 就标着 sRGB，所以此前按「反射贴图是线性数据」去要 LINEAR 的结果是
// 「解码失败 + 静默降级为无反射」：画面看着正常，IBL 实际完全没生效。
//
// 而 DFD 的字节布局属于 Filament / KTX 的实现细节，在这里再抄一份解析逻辑
// 只会随上游演进失效（实测其字段对齐与规范文档并不一致）。所以改成以
// Ktx2Reader 自己的判断为准：先按调用方语义偏好试一次（IBL 反射贴图偏
// LINEAR，天空盒偏 sRGB），被拒就换另一种再试。每次都用**全新 reader** ——
// 失败之后 reader 的状态没有契约保证。两次都失败才是真坏文件，返回 nullptr
// 交给调用方降级。
Texture *loadKtx2Texture(Engine &engine, const std::vector<uint8_t> &bytes,
                         ktxreader::Ktx2Reader::TransferFunction preferred)
{
    // 本构建的 Ktx2Reader **不支持 cubemap**，这是硬事实而非猜测：libktxreader.a
    // 的 Ktx2Reader.cpp.o 里就有字符串 "Cubemaps are not yet supported."，实测对
    // faceCount=6 的文件 asyncCreate 一律返回 nullptr，而同一张图改成 2D
    // （faceCount=1）则成功。IBL 反射贴图与天空盒按定义必须是 cubemap，因此这条
    // 分支对真实资产**永远**会失败。
    //
    // 提前判掉的目的不是省那点解码时间，而是给调用方一个准确原因 —— 否则只会
    // 看到笼统的「解码失败」，很容易再次误判成文件损坏或编码不兼容。
    //
    // 保留函数本体而不删除：上游若放开 cubemap 支持，调用点无需改动即可自动生效。
    const uint32_t faces = ktx2FaceCount(bytes);
    if (faces > 1)
    {
        std::fprintf(stderr,
                     "[filament] KTX2 cubemap 不被本构建的 Ktx2Reader 支持：\"Cubemaps "
                     "are not yet supported.\"（faceCount=%u）。IBL 反射贴图与天空盒请改用 "
                     "KTX1（.ktx，cmgen 产物）。\n",
                     faces);
        return nullptr;
    }

    using TransferFunction = ktxreader::Ktx2Reader::TransferFunction;
    const TransferFunction fallback = preferred == TransferFunction::sRGB
                                          ? TransferFunction::LINEAR
                                          : TransferFunction::sRGB;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        // 第一次是预期内的试探：文件若与偏好相反，它必然失败并往 stderr 打一条
        // 冲突说明。那条不是真错误，静音掉；第二次（回退）不静音，所以两次都
        // 失败时调用方仍能看到原因。
        ktxreader::Ktx2Reader reader(engine, /*quiet=*/attempt == 0);
        // 必须先 requestFormat() 声明想要的内部格式，否则 load() 一律返回
        // nullptr —— Ktx2Reader 只转码到被显式请求过的格式，不会替你猜。
        registryFormatsForKtx2(reader);
        const TransferFunction transfer = attempt == 0 ? preferred : fallback;
        if (Texture *texture = reader.load(bytes.data(), bytes.size(), transfer))
            return texture;
    }
    return nullptr;
}

// KTX1 解码的两步都持有**弱引用**，所以字节必须活到 GPU 上传完成。
//
// 两个事实决定了这个结构体的存在，缺一不可：
//   1. Ktx1Bundle 不拷贝输入。它的构造函数是「deserializing the given data」，
//      而 getBlob() 的文档原话是 "Retrieves a weak reference to a given data
//      blob" —— 解析完头部之后，bundle 只记着指向调用方那块内存的指针。
//   2. 上传是异步的。Ktx1Reader::createTexture 立刻返回 Texture*，而它那个
//      callback 参数的语义是 "Gets called after all texture data has been
//      uploaded to the GPU"。也就是说函数返回时字节**还没**被读走。
//
// 把 `std::vector` 留在栈上、函数一返回就析构，等于让 Filament 去上传一块已经
// 归还给分配器的内存：不崩就是运气，表现是纹理全黑/花屏 —— 曾经 IBL 与天空盒
// 一起黑掉就是这个原因。所以把 buffer 和 bundle 打包成一个堆对象，交给上传
// 回调在正确的时刻回收。旧路径 src/engine.hpp:159 的 KtxPack 是同一个模式。
struct Ktx1Pack
{
    std::vector<uint8_t> bytes;   // 必须先于 bundle 声明：bundle 引用它
    image::Ktx1Bundle bundle;
};

// 从 KTX1 字节建一个 Texture。bytes 按值传入并由调用点 move，不发生拷贝。
//
// srgb：天空盒要 sRGB 传递函数，IBL 的反射贴图是线性数据，不编码。
// 返回 nullptr 表示解析或创建失败 —— 这种情况下 Filament 不会调用回调，
// 由本函数直接回收。
Texture *createKtx1Texture(Engine *engine, std::vector<uint8_t> bytes, bool srgb)
{
    // 先验魔数。Ktx1Bundle 构造里的 Precondition 会把「坏输入」升级成进程级
    // abort（utils::PreconditionPanic 穿过 main），而这里的契约是返回 nullptr。
    if (!hasIdentifier(bytes, kKtx1Identifier))
        return nullptr;

    // 先把指针取出来再 move：std::vector 被移动之后处于「有效但未指定」状态，
    // 对它再调 data() 不保证还是原来那块地址（标准只保证不崩）。
    const uint8_t *data = bytes.data();
    const uint32_t size = static_cast<uint32_t>(bytes.size());

    auto *pack = new Ktx1Pack{std::move(bytes), image::Ktx1Bundle{data, size}};

    Texture *texture = ktxreader::Ktx1Reader::createTexture(
        engine, pack->bundle, srgb,
        [](void *user) { delete static_cast<Ktx1Pack *>(user); }, pack);

    if (!texture)
        delete pack;

    return texture;
}

// 1x1 中性纹理的填充值。
//
// 这些值不是随便选的 —— 材质里纹理是与 factor 逐分量相乘的（见 src/mat/*.mat
// 的 fragment 段），所以「没有贴图」必须由**身份元**表示，否则 factor 会失效：
//   baseColor  白   → baseColorFactor 原样生效
//   emissive   白   → 这一步最容易写错。填黑会让 emissiveFactor 被乘成 0，
//                      症状是「材质明明设了自发光却完全不亮」，看起来像光照坏了
//   roughness  白   → roughnessFactor 原样生效
//   metallic   白   → metallicFactor 原样生效
//   ao         白   → 无遮挡
//   normal     (0.5,0.5,1) 编码切线空间 (0,0,1)，即「不扰动」
inline void fillNeutralTexel(render::TextureUsage usage, uint8_t rgba[4]) noexcept
{
    rgba[3] = 255;
    switch (usage)
    {
    case render::TextureUsage::Normal:
        rgba[0] = 128;
        rgba[1] = 128;
        rgba[2] = 255;
        break;
    default:
        rgba[0] = rgba[1] = rgba[2] = 255;
        break;
    }
}

inline filament::math::float3 toFilamentVec3(const math::Vec3 &v) noexcept
{
    return {v.x, v.y, v.z};
}

inline filament::math::mat4f toFilamentMat4(const math::Mat4 &m) noexcept
{
    filament::math::mat4f out;
    // math::TMatHelpers 只提供 const 版本的 asArray()（TMatHelpers.h:675），
    // 这里的写入是就地构造 out 的合法用法，所以显式去掉 const。
    std::memcpy(const_cast<float *>(out.asArray()), m.data(), sizeof(float) * 16);
    return out;
}

} // namespace

// =====================================================================
// 构造 / 析构
// =====================================================================
//
// 析构是本类与旧 engine.hpp 最重要的行为差异。旧实现是
//   ~Engine() noexcept = default;
// 一个 Engine 成员指针而已 —— Filament 的对象树（Scene/View/Renderer/
// Renderable/Material/Texture/VertexBuffer...）**全部泄漏**，进程退出时靠
// 驱动回收。那在长生命周期进程里是不可接受的。
//
// 这里的顺序按依赖倒序，且每一步都容忍空指针（构造中途失败时对象处于
// 半初始化状态，析构仍会被调用）。这个「构造失败也要能安全析构」的性质
// 是析构函数全部写成 if (p) 的原因，不是防御性编程洁癖。

FilamentRenderer::FilamentRenderer(std::string assetRoot, MaterialPackPaths packs, Engine::Backend backend)
    : assetRoot_(std::move(assetRoot)), packs_(std::move(packs))
{
    engine_ = Engine::create(backend);
    if (!engine_)
        throw std::runtime_error("FilamentRenderer: Engine::create failed");

    renderer_ = engine_->createRenderer();
    scene_ = engine_->createScene();
    view_ = engine_->createView();

    if (!renderer_ || !scene_ || !view_)
        throw std::runtime_error("FilamentRenderer: failed to create renderer/scene/view");

    // 相机需要一个实体承载组件；Engine 没有 destroy(Camera*)，只有
    // destroyCameraComponent(Entity) —— 销毁路径见析构。
    cameraEntity_ = utils::EntityManager::get().create();
    camera_ = engine_->createCamera(cameraEntity_);
    if (!camera_)
        throw std::runtime_error("FilamentRenderer: failed to create camera");

    // 相机实体也要有 Transform 组件，否则 setModelMatrix 无处安放。
    engine_->getTransformManager().create(cameraEntity_);

    view_->setScene(scene_);
    view_->setCamera(camera_);
    view_->setFrustumCullingEnabled(true);

    // Filament 的 PBR 光照是 HDR；保留后处理的色调映射，避免亮部在显示输出时
    // 直接裁成白色。离屏读回的色彩空间仍由 readbackImage 明确标记。
    view_->setPostProcessingEnabled(false);

    if (!loadMaterialPacks())
    {
        // 构造失败：让析构负责清理已经建好的 Engine 部分。
        throw std::runtime_error("FilamentRenderer: failed to load material packs under '" + assetRoot_ + "'");
    }
}

// 销毁一个「登记在 Scene 名单里」的实体：必须先摘名单，再销毁组件。
//
// 顺序反过来（先 destroy 再 remove）会把一个已回收的 Entity 留在 Scene 名单里，
// Filament 下一帧遍历名单时读到的就是已被复用的 Entity id —— 轻则幽灵物体，
// 重则读到另一个实体的组件。所有实体销毁都必须走这里，不要直接调
// engine_->destroy(entity)。
//
// Scene::remove 对「不在名单里」和「已失效」的实体是显式 ignore
// （见 libs/filament/include/filament/Scene.h:129），所以对建了一半失败的
// 孤立实体同样安全。
void FilamentRenderer::destroyEntity(utils::Entity entity)
{
    if (entity.isNull())
        return;

    if (scene_)
        scene_->remove(entity);
    if (engine_)
        engine_->destroy(entity);
}

FilamentRenderer::~FilamentRenderer()
{
    if (!engine_)
        return;

    // ---- 1. 世界翻译出的实体 ----
    // 先销毁 Renderable 组件所在实体，再销毁几何/材质，否则 Filament 会
    // 在 destroy(Material) 时因为「仍有 MaterialInstance 存活」而报错。
    for (auto &kv : primitives_)
        destroyEntity(kv.second.entity);
    primitives_.clear();

    for (utils::Entity e : lightEntities_)
        destroyEntity(e);
    lightEntities_.clear();

    // ---- 2. 材质实例必须先于材质 ----
    materials_.forEach(
        [this](const MaterialRecord &rec)
        {
            if (rec.instance)
                engine_->destroy(rec.instance);
        });
    materials_.clear();

    // ---- 3. 纹理（去重表与 SlotMap 是两套记账，都要清）----
    // textureBySource_ 里的指针与 textures_ 中 ownsTexture=true 的项可能重叠，
    // 所以先按 SlotMap 的真实所有权销毁，再清空去重表（只清表不销毁）。
    textures_.forEach(
        [this](const TextureRecord &rec)
        {
            if (rec.texture && rec.ownsTexture)
                engine_->destroy(rec.texture);
        });
    textures_.clear();
    textureBySource_.clear();

    // 中性纹理不在 SlotMap 里，单独记一份 —— 见 neutralTextures_。
    for (auto &kv : neutralTextures_)
        engine_->destroy(kv.second);
    neutralTextures_.clear();

    meshes_.forEach(
        [this](const MeshRecord &rec)
        {
            if (rec.vb)
                engine_->destroy(rec.vb);
            if (rec.ib)
                engine_->destroy(rec.ib);
        });
    meshes_.clear();

    // ---- 4. 材质包（实例已全部销毁，现在可以安全销毁材质本体）----
    if (litAormMaterial_)
        engine_->destroy(litAormMaterial_);
    if (litMaterial_)
        engine_->destroy(litMaterial_);
    if (unlitMaterial_)
        engine_->destroy(unlitMaterial_);
    litAormMaterial_ = litMaterial_ = unlitMaterial_ = nullptr;

    // ---- 5. 环境 / 离屏 / 相机 / 视图 ----
    // 天空盒必须**先于**它的环境贴图销毁：Skybox 内部会引用那张 Texture，
    // 顺序反了就是经典的「先释放被引用者」。
    if (skybox_)
        engine_->destroy(skybox_);
    skybox_ = nullptr;
    if (skyboxEnvironment_)
        engine_->destroy(skyboxEnvironment_);
    skyboxEnvironment_ = nullptr;

    // IBL 表里可能有多份（loadIblFromKtx 每次新建一张）。indirectLight_
    // 总是指向表中某一份（applyEnvironment 只从 ibls_ 取），所以这里
    // **不能**单独销毁它 —— 那会和下面的表清理撞成双重释放。只解引用。
    indirectLight_ = nullptr;
    ibls_.forEach(
        [this](const IblRecord &rec)
        {
            if (rec.indirectLight)
                engine_->destroy(rec.indirectLight);
            if (rec.reflections && rec.ownsReflections)
                engine_->destroy(rec.reflections);
        });
    ibls_.clear();

    if (offscreenRt_)
        engine_->destroy(offscreenRt_);
    if (offscreenColor_)
        engine_->destroy(offscreenColor_);
    if (offscreenDepth_)
        engine_->destroy(offscreenDepth_);
    offscreenRt_ = nullptr;
    offscreenColor_ = nullptr;
    offscreenDepth_ = nullptr;

    if (camera_)
        engine_->destroyCameraComponent(cameraEntity_);
    if (cameraEntity_)
        engine_->destroy(cameraEntity_);
    camera_ = nullptr;
    cameraEntity_ = {};

    if (view_)
        engine_->destroy(view_);
    if (scene_)
        engine_->destroy(scene_);
    if (renderer_)
        engine_->destroy(renderer_);
    if (swapChain_)
        engine_->destroy(swapChain_);
    view_ = nullptr;
    scene_ = nullptr;
    renderer_ = nullptr;
    swapChain_ = nullptr;

    // ---- 6. 最后才是 Engine ----
    Engine::destroy(&engine_);
}

// =====================================================================
// 材质包
// =====================================================================
std::string FilamentRenderer::resolvePath(const std::string &relativePath) const
{
    if (relativePath.empty() || relativePath.front() == '/')
        return relativePath;
    if (assetRoot_.empty() || assetRoot_ == ".")
        return relativePath;
    if (assetRoot_.back() == '/')
        return assetRoot_ + relativePath;
    return assetRoot_ + "/" + relativePath;
}

bool FilamentRenderer::loadMaterialPack(const std::string &relativePath, Material *&out)
{
    const std::string path = resolvePath(relativePath);
    std::vector<uint8_t> payload;
    if (!readFileBytes(path, payload))
    {
        std::fprintf(stderr, "[filament] cannot read material pack: %s\n", path.c_str());
        return false;
    }

    out = Material::Builder()
              .package(payload.data(), payload.size())
              .build(*engine_);
    return out != nullptr;
}

bool FilamentRenderer::loadMaterialPacks()
{
    if (!loadMaterialPack(packs_.unlit, unlitMaterial_))
        return false;
    if (!loadMaterialPack(packs_.lit, litMaterial_))
        return false;
    if (!loadMaterialPack(packs_.litAorm, litAormMaterial_))
        return false;
    return true;
}

// =====================================================================
// 网格
// =====================================================================
//
// 本函数的核心是 TBN 的重新编码，值得单独说明。
//
// **问题**：Filament 的 VertexAttribute 枚举里根本没有 NORMAL
// （POSITION=0, TANGENTS=1, COLOR=2, UV0=3）。法线必须从 TANGENTS 属性
// 携带的「打包四元数」里还原。也就是说 Filament 要的不是一个切线向量，
// 而是把 (tangent, bitangent, normal) 这个正交基整体编码成 quatf。
//
// **而引擎侧的 Vertex 存的是 glTF 标准表达**：tangent.xyz 是切线、tangent.w
// 是手性 ±1，副切线 = cross(n, t) * w。见 asset/mesh_data.hpp 的注释 ——
// 那里刻意不存 Filament 的编码，因为那是后端细节。
//
// **所以这里必须做转换**：展开 → 正交化 → packTangentFrame。
//
// 这个转换做错时的症状很有欺骗性：模型会整个变黑或法线方向诡异。而因为
// TANGENTS 只是「另一个顶点属性」，不做这个转换、直接把 (x,y,z,w) 当
// 切线传进去**在 unlit 材质下完全不报错也不出错**（unlit 不需要法线），
// 只有切到 lit 材质才会暴露。别在这里省事。
//
// 存储格式用 float（而非 Filament 默认的 snorm16）：
// storageSize = sizeof(float) 对应 AttributeType::FLOAT4，精度足够且省去
// 与 SHORT4 的量化误差打交道。代价是每顶点多 8 字节。

bool FilamentRenderer::buildVertexBuffer(const asset::MeshData &mesh, VertexBuffer *&vb, IndexBuffer *&ib)
{
    vb = nullptr;
    ib = nullptr;

    const size_t vertexCount = mesh.vertices.size();
    const size_t indexCount = mesh.indices.size();
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0)
    {
        std::fprintf(stderr, "[filament] mesh '%s' rejected: %zu verts / %zu indices\n",
                     mesh.name.c_str(), vertexCount, indexCount);
        return false;
    }

    // 中间缓冲的布局在 GpuVertex 中固定为 48 字节 stride：
    // POSITION FLOAT3 @ 0, TANGENTS FLOAT4 @ 16, UV0 FLOAT2 @ 32。
    std::vector<GpuVertex> packed(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i)
        packGpuVertex(mesh.vertices[i], packed[i]);

    vb = VertexBuffer::Builder()
             .vertexCount(static_cast<uint32_t>(vertexCount))
             .bufferCount(1)
             .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(GpuVertex))
             .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, 16, sizeof(GpuVertex))
             .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 32, sizeof(GpuVertex))
             .build(*engine_);

    if (!vb)
        return false;

    // packed 是局部 vector，函数返回即析构 —— 必须交给 Filament 一份自己拥有的拷贝。
    const size_t packedBytes = packed.size() * sizeof(GpuVertex);
    void *ownedVertices = copyToOwnedHeap(packed.data(), packedBytes);
    if (!ownedVertices)
    {
        engine_->destroy(vb);
        vb = nullptr;
        return false;
    }

    vb->setBufferAt(*engine_, 0,
                    VertexBuffer::BufferDescriptor(ownedVertices, packedBytes, freeOwnedBuffer));

    ib = IndexBuffer::Builder()
             .indexCount(static_cast<uint32_t>(indexCount))
             .bufferType(IndexBuffer::IndexType::UINT)
             .build(*engine_);
    if (!ib)
    {
        // destroy(vb) 会让 Filament 调用 ownedVertices 的释放回调，这里不必再 free。
        engine_->destroy(vb);
        vb = nullptr;
        return false;
    }

    // mesh.indices 指向调用方的 MeshData：它活得够久是调用方的运气，不是这里的
    // 契约。同样拷一份给自己。
    void *ownedIndices = copyToOwnedHeap(mesh.indices.data(), indexCount * sizeof(uint32_t));
    if (!ownedIndices)
    {
        engine_->destroy(vb);
        engine_->destroy(ib);
        vb = nullptr;
        ib = nullptr;
        return false;
    }

    ib->setBuffer(*engine_,
                  IndexBuffer::BufferDescriptor(ownedIndices, indexCount * sizeof(uint32_t),
                                                freeOwnedBuffer));

    return true;
}

MeshHandle FilamentRenderer::createMesh(const asset::MeshData &mesh)
{
    VertexBuffer *vb = nullptr;
    IndexBuffer *ib = nullptr;
    if (!buildVertexBuffer(mesh, vb, ib))
    {
        if (vb)
            engine_->destroy(vb);
        if (ib)
            engine_->destroy(ib);
        return {};
    }

    MeshRecord rec;
    rec.vb = vb;
    rec.ib = ib;
    rec.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    rec.indexCount = static_cast<uint32_t>(mesh.indices.size());
    rec.bounds = mesh.bounds;
    rec.skinned = mesh.skinned();
    return meshes_.insert(rec);
}

bool FilamentRenderer::updateMeshVertices(MeshHandle handle,
                                          const std::vector<asset::Vertex> &vertices)
{
    MeshRecord *rec = meshes_.get(handle);
    if (rec == nullptr || rec->vb == nullptr || vertices.size() != rec->vertexCount)
        return false;

    std::vector<GpuVertex> packed(vertices.size());
    math::AABB bounds;
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        packGpuVertex(vertices[i], packed[i]);
        bounds.expand(vertices[i].position);
    }
    if (!bounds.valid)
        return false;

    const size_t bytes = packed.size() * sizeof(GpuVertex);
    void *ownedVertices = copyToOwnedHeap(packed.data(), bytes);
    if (ownedVertices == nullptr)
        return false;

    rec->vb->setBufferAt(*engine_, 0,
                         VertexBuffer::BufferDescriptor(ownedVertices, bytes, freeOwnedBuffer));
    rec->bounds = bounds;

    RenderableManager &renderables = engine_->getRenderableManager();
    for (const auto &[key, primitive] : primitives_)
    {
        (void)key;
        if (primitive.mesh != handle)
            continue;
        const auto instance = renderables.getInstance(primitive.entity);
        if (instance)
            renderables.setAxisAlignedBoundingBox(instance, toFilamentBox(bounds));
    }

    return true;
}

void FilamentRenderer::destroy(MeshHandle handle)
{
    MeshRecord *rec = meshes_.get(handle);
    if (!rec)
        return;

    // 先摘掉引用这块几何的实体，否则 Filament 侧会留下悬垂的 Renderable。
    for (auto it = primitives_.begin(); it != primitives_.end();)
    {
        if (it->second.mesh == handle)
        {
            destroyEntity(it->second.entity);
            it = primitives_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (rec->vb)
        engine_->destroy(rec->vb);
    if (rec->ib)
        engine_->destroy(rec->ib);
    meshes_.erase(handle);
}

// =====================================================================
// 纹理
// =====================================================================
Texture *FilamentRenderer::uploadImage(const asset::ImageData &image, render::TextureUsage usage)
{
    if (!image.valid())
        return nullptr;

    // 色彩空间由**用途**决定，不由 ImageData::colorSpace 决定。
    // 理由是用途是语义（这是 baseColor），而 colorSpace 只是这张图碰巧
    // 被解码成了什么。以用途为准可以避免「调用方忘了标 colorSpace」这类错误。
    const bool srgb = usage == render::TextureUsage::BaseColor ||
                      usage == render::TextureUsage::Emissive;

    Texture *tex = Texture::Builder()
                       .width(image.width)
                       .height(image.height)
                       .levels(1)
                       .sampler(Texture::Sampler::SAMPLER_2D)
                       .format(srgb ? Texture::InternalFormat::SRGB8_A8
                                    : Texture::InternalFormat::RGBA8)
                       .usage(Texture::Usage::SAMPLEABLE | Texture::Usage::UPLOADABLE)
                       .build(*engine_);
    if (!tex)
        return nullptr;

    // image 由调用方持有，上传却是异步的：拷一份给自己，释放权交给 Filament。
    void *ownedPixels = copyToOwnedHeap(image.pixels.data(), image.pixels.size());
    if (!ownedPixels)
    {
        engine_->destroy(tex);
        return nullptr;
    }

    tex->setImage(*engine_, 0,
                  Texture::PixelBufferDescriptor(
                      ownedPixels, image.pixels.size(),
                      ::filament::backend::PixelDataFormat::RGBA,
                      ::filament::backend::PixelDataType::UBYTE,
                      freeOwnedBuffer));

    // levels(1)：不生成 mipmap。
    //
    // 这是一个**有意的取舍**，不是遗漏。生成 mip 需要 engine 侧额外的
    // blit 通路（Texture::generateMipmaps）或离线 mipgen，而当前 demo 的
    // 观察距离下 mip 只影响远处纹理的闪烁。选择是：先把「纹理真的被采样到」
    // 这件事做对，再谈采样质量。于是采样器用 LINEAR（不是
    // LINEAR_MIPMAP_LINEAR）—— 后者在没有 mip 的纹理上行为未定义。
    return tex;
}

Texture *FilamentRenderer::acquireTexture(const asset::TextureSlot &slot, render::TextureUsage usage)
{
    if (slot.empty())
        return nullptr;

    // 按 source 去重。source 为空时退化为「每处各上传一份」——
    // 那样虽然浪费显存，但不会把两张不同的图错误地当成同一张。
    if (!slot.source.empty())
    {
        const auto it = textureBySource_.find(slot.source);
        if (it != textureBySource_.end())
            return it->second;
    }

    Texture *tex = uploadImage(*slot.image, usage);
    if (!tex)
        return nullptr;

    // 两张表分工不同，**两张都必须写**：
    //   textures_         所有权表 —— 析构与 destroy(TextureHandle) 靠它释放；
    //   textureBySource_  查询表   —— 只做 source → 纹理 的去重查询。
    //
    // 这里曾经只写查询表（注释还声称插了一条 ownsTexture=false 的见证记录，
    // 但代码里并没有），而析构遍历的是 textures_、对查询表只 clear() ——
    // 结果是 createMaterial 上传的每一张贴图都无人释放。两表允许重叠，
    // destroy(TextureHandle) 会同时清理两边，所以重复登记是安全的。
    TextureRecord rec;
    rec.texture = tex;
    rec.usage = usage;
    rec.ownsTexture = true;
    textures_.insert(rec);

    if (!slot.source.empty())
        textureBySource_.emplace(slot.source, tex);
    return tex;
}

TextureHandle FilamentRenderer::createTexture(const asset::ImageData &image, render::TextureUsage usage)
{
    Texture *tex = uploadImage(image, usage);
    if (!tex)
        return {};

    TextureRecord rec;
    rec.texture = tex;
    rec.usage = usage;
    rec.ownsTexture = true;
    return textures_.insert(rec);
}

void FilamentRenderer::destroy(TextureHandle handle)
{
    TextureRecord *rec = textures_.get(handle);
    if (!rec)
        return;
    if (rec->texture && rec->ownsTexture)
    {
        // 去重表里若还指着它，一并摘掉，避免下个材质拿到已销毁的纹理。
        for (auto it = textureBySource_.begin(); it != textureBySource_.end();)
            it = (it->second == rec->texture) ? textureBySource_.erase(it) : std::next(it);
        engine_->destroy(rec->texture);
    }
    textures_.erase(handle);
}

// 1x1 中性纹理，按用途缓存。
// 这是私有成员 neutralTextures_ —— 声明见 .hpp。
Texture *FilamentRenderer::neutralTexture(render::TextureUsage usage)
{
    const auto it = neutralTextures_.find(usage);
    if (it != neutralTextures_.end())
        return it->second;

    uint8_t texel[4];
    fillNeutralTexel(usage, texel);

    // texel 是栈数组，setImage 返回后即失效 —— 同样是异步读，必须拷到堆。
    void *ownedTexel = copyToOwnedHeap(texel, sizeof(texel));
    if (!ownedTexel)
        return nullptr;

    const bool srgb = usage == render::TextureUsage::BaseColor ||
                      usage == render::TextureUsage::Emissive;

    Texture *tex = Texture::Builder()
                       .width(1)
                       .height(1)
                       .levels(1)
                       .sampler(Texture::Sampler::SAMPLER_2D)
                       .format(srgb ? Texture::InternalFormat::SRGB8_A8
                                    : Texture::InternalFormat::RGBA8)
                       .usage(Texture::Usage::SAMPLEABLE | Texture::Usage::UPLOADABLE)
                       .build(*engine_);
    if (!tex)
    {
        std::free(ownedTexel);
        return nullptr;
    }

    tex->setImage(*engine_, 0,
                  Texture::PixelBufferDescriptor(
                      ownedTexel, sizeof(texel),
                      ::filament::backend::PixelDataFormat::RGBA,
                      ::filament::backend::PixelDataType::UBYTE,
                      freeOwnedBuffer));
    neutralTextures_.emplace(usage, tex);
    return tex;
}

// =====================================================================
// 材质
// =====================================================================
//
// Filament 要求材质声明的**每一个** sampler 都被 setParameter 设上，
// 否则运行时会打印 "sampler parameters not set" 并在着色器里读到垃圾。
// 所以这里的原则是：宁可给一张 1x1 中性纹理，也不留空 —— 见 fillNeutralTexel。
MaterialHandle FilamentRenderer::createMaterial(const asset::MaterialDesc &desc)
{
    Material *pack = nullptr;
    switch (desc.model)
    {
    case asset::ShadingModel::Unlit:
        pack = unlitMaterial_;
        break;
    case asset::ShadingModel::LitAORM:
        pack = litAormMaterial_;
        break;
    case asset::ShadingModel::Lit:
    default:
        pack = litMaterial_;
        break;
    }
    if (!pack)
        return {};

    MaterialInstance *mi = pack->createInstance(desc.name.c_str());
    if (!mi)
        return {};

    const filament::math::float4 baseColor{desc.baseColorFactor.x, desc.baseColorFactor.y,
                                           desc.baseColorFactor.z, desc.baseColorFactor.w};

    Texture *baseTex = acquireTexture(desc.baseColor, render::TextureUsage::BaseColor);
    if (!baseTex)
        baseTex = neutralTexture(render::TextureUsage::BaseColor);
    mi->setParameter("baseColor", baseTex, TextureSampler{});

    if (desc.model == asset::ShadingModel::Unlit)
    {
        // unlit.mat 只声明了 baseColor + baseColorFactor 两个参数（见 src/mat/unlit.mat），
        // 多余地设 emissive / normal 会触发 "parameter not found" 警告。
        mi->setParameter("baseColorFactor", baseColor);
    }
    else
    {
        mi->setParameter("baseColorFactor", baseColor);

        const filament::math::float4 emissive{desc.emissiveFactor.x, desc.emissiveFactor.y,
                                              desc.emissiveFactor.z, desc.emissiveFactor.w};
        mi->setParameter("emissiveFactor", emissive);

        Texture *emissiveTex = acquireTexture(desc.emissive, render::TextureUsage::Emissive);
        if (!emissiveTex)
            emissiveTex = neutralTexture(render::TextureUsage::Emissive);
        mi->setParameter("emissive", emissiveTex, TextureSampler{});

        Texture *normalTex = acquireTexture(desc.normal, render::TextureUsage::Normal);
        if (!normalTex)
            normalTex = neutralTexture(render::TextureUsage::Normal);
        mi->setParameter("normal", normalTex, TextureSampler{});

        if (desc.model == asset::ShadingModel::LitAORM)
        {
            // aorm 是打包纹理：R=AO, G=Roughness, B=Metallic（见 src/mat/lit_aorm.mat）。
            // 引擎侧的 MaterialDesc 把它们拆成三个独立 slot，所以这里必须合成。
            const asset::ImagePtr packed = packAormImage(desc);
            Texture *aormTex = nullptr;
            if (packed)
            {
                aormTex = uploadImage(*packed, render::TextureUsage::AmbientOcclusion);
                // 合成出来的 aorm 图没有 source 字符串可拿去重，所以不能走
                // acquireTexture；但必须自己登记进所有权表 —— 否则它两张表
                // 都不在，析构根本看不到它，每个 LitAORM 材质都漏一张纹理。
                if (aormTex)
                {
                    TextureRecord aormRec;
                    aormRec.texture = aormTex;
                    aormRec.usage = render::TextureUsage::AmbientOcclusion;
                    aormRec.ownsTexture = true;
                    textures_.insert(aormRec);
                }
            }
            if (!aormTex)
                aormTex = neutralTexture(render::TextureUsage::AmbientOcclusion);
            mi->setParameter("aorm", aormTex, TextureSampler{});
        }
        else
        {
            Texture *aoTex = acquireTexture(desc.ambientOcclusion, render::TextureUsage::AmbientOcclusion);
            if (!aoTex)
                aoTex = neutralTexture(render::TextureUsage::AmbientOcclusion);
            mi->setParameter("ambientOcclusion", aoTex, TextureSampler{});

            Texture *roughTex = acquireTexture(desc.roughness, render::TextureUsage::MetallicRoughness);
            if (!roughTex)
                roughTex = neutralTexture(render::TextureUsage::MetallicRoughness);
            mi->setParameter("roughness", roughTex, TextureSampler{});

            Texture *metalTex = acquireTexture(desc.metallic, render::TextureUsage::MetallicRoughness);
            if (!metalTex)
                metalTex = neutralTexture(render::TextureUsage::MetallicRoughness);
            mi->setParameter("metallic", metalTex, TextureSampler{});
        }

        mi->setParameter("roughnessFactor", desc.roughnessFactor);
        mi->setParameter("metallicFactor", desc.metallicFactor);
    }

    mi->setCullingMode(desc.doubleSided ? ::filament::backend::CullingMode::NONE
                                        : ::filament::backend::CullingMode::BACK);
    return materials_.insert(MaterialRecord{pack, mi, desc.model, desc.doubleSided});
}

void FilamentRenderer::destroy(MaterialHandle handle)
{
    MaterialRecord *rec = materials_.get(handle);
    if (!rec)
        return;

    for (auto it = primitives_.begin(); it != primitives_.end();)
    {
        if (it->second.material == handle)
        {
            destroyEntity(it->second.entity);
            it = primitives_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (rec->instance)
        engine_->destroy(rec->instance);
    materials_.erase(handle);
}

// =====================================================================
// 环境 / 场景同步用的局部辅助
// =====================================================================
namespace
{

// Filament 不接受半边长 <= 0 的包围盒（PreConditionPanic）。引擎侧的 AABB
// 允许退化（甚至 valid == false 的空盒），所以这里统一兜底：无效就用单位盒，
// 某一轴退化就撑到最小厚度。
inline Box toFilamentBox(const math::AABB &aabb) noexcept
{
    filament::math::float3 mn{-1.0f, -1.0f, -1.0f};
    filament::math::float3 mx{1.0f, 1.0f, 1.0f};
    if (aabb.valid)
    {
        mn = filament::math::float3{aabb.min.x, aabb.min.y, aabb.min.z};
        mx = filament::math::float3{aabb.max.x, aabb.max.y, aabb.max.z};
    }
    constexpr float kMinHalfExtent = 1.0e-3f;
    for (int i = 0; i < 3; ++i)
    {
        if (mx[i] - mn[i] < 2.0f * kMinHalfExtent)
        {
            const float c = 0.5f * (mx[i] + mn[i]);
            mn[i] = c - kMinHalfExtent;
            mx[i] = c + kMinHalfExtent;
        }
    }
    Box box;
    box.set(mn, mx);
    return box;
}

// core/math.hpp 的 Mat4 没有比较运算符（刻意如此：浮点矩阵没有「相等」的
// 干净定义）。这里要的只是「和上次上传的一不一样」，逐字节比就够 ——
// 比不出差异就不会有上传，这是纯收益。
inline bool sameMatrix(const math::Mat4 &a, const math::Mat4 &b) noexcept
{
    return std::memcmp(a.data(), b.data(), sizeof(float) * 16) == 0;
}

inline bool anyNonZero(const math::Vec3 bands[9]) noexcept
{
    for (int i = 0; i < 9; ++i)
    {
        if (bands[i].x != 0.0f || bands[i].y != 0.0f || bands[i].z != 0.0f)
            return true;
    }
    return false;
}

inline bool sameBands(const math::Vec3 a[9], const math::Vec3 b[9]) noexcept
{
    for (int i = 0; i < 9; ++i)
    {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
            return false;
    }
    return true;
}

// 灯光的单位映射。
// Engine 侧的 LightDesc::intensity 是无量纲乘数（CPU / ASCII 后端直接拿它
// 乘颜色），Filament 要的却是物理量：directional 用 lux，point 用 lumen。
// 两边语义不同，必须在这里显式换算，否则「同一份场景换个后端」亮度会差
// 三四个数量级 —— 这正是抽象该吸收的差异，不是让上层去调。
constexpr float kLuxPerIntensityUnit = 100000.0f;    // 1.0 ≈ 晴天直射日光
constexpr float kLumenPerIntensityUnit = 1000.0f;    // 1.0 ≈ 一只普通灯泡

// IBL 强度：30000 是 Filament **IndirectLight 的默认 lux 值本身**，不是
// 「相对倍率 → lux」的换算常数（IndirectLight.h:266）。把它当换算常数用，
// 等于把 1.0 的相对倍率放大成 30000 lux；而场景上一旦挂了 IndirectLight，
// Skybox 的 intensity 就被它覆盖（Skybox.h:108），天空盒自发光随之放大
// 30000 倍 —— 实测就是满屏全白（mean 1.0000）。
// 这里取 1.0：引擎侧的 iblIntensity 是相对倍率，1.0 = 单位曝光下的标准环境。
constexpr float kIblIntensityUnit = 1.0f;

// readPixels 的 CPU 侧回读是异步的（驱动完成拷贝后才回调）。这块内存的
// 所有权要在「主线程」和「回调」之间裁决。phase 是唯一的裁决点：
//   0 = 尚未完成   1 = 回调已送达（主线程接手）   2 = 主线程已放弃
//
// 裁决的规矩只有一条：**释放权只在回调手里**。Filament 保证 descriptor 的
// callback 一定会被调用（backend/BufferDescriptor.h:44），所以回调是那个
// 「无论如何都会到场」的一方；主线程可能先超时走人，不适合当释放者。
// 主线程若在这里 free/delete，回调稍后仍会访问同一个 job —— 那是 UAF 加
// 双重释放，而且只在慢驱动上才会踩到。
struct ReadbackJob
{
    std::atomic<int> phase{0};
    uint8_t *buffer = nullptr;
    size_t size = 0;
};

void readbackCallback(void *buffer, size_t size, void *user)
{
    auto *job = static_cast<ReadbackJob *>(user);
    job->buffer = static_cast<uint8_t *>(buffer);
    job->size = size;
    std::atomic_thread_fence(std::memory_order_release);

    int expected = 0;
    if (job->phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        // 主线程已接手：它读完 pixels 后负责 free(buffer) 与 delete job。
        return;
    }

    // 主线程已放弃（phase == 2）且承诺不再触碰 job：由回调收尾。
    std::free(buffer);
    delete job;
}

} // namespace

// =====================================================================
// 交换链 / 离屏目标
// =====================================================================

bool FilamentRenderer::attachSwapChain(void *nativeWindow, uint64_t flags)
{
    if (!engine_ || !renderer_)
        return false;

    // 换目标的顺序是「先拆旧的，再建新的」。headless 与 on-screen 交换链在
    // Filament 里是同一类对象，漏掉这一步会在反复切换（离屏抓帧 → 上屏）时
    // 把整条链泄漏在显存里。
    if (swapChain_)
    {
        engine_->destroy(swapChain_);
        swapChain_ = nullptr;
    }

    swapChain_ = engine_->createSwapChain(nativeWindow, flags);
    if (!swapChain_)
    {
        std::fprintf(stderr, "[filament] createSwapChain 失败（nativeWindow=%p）\n", nativeWindow);
        return false;
    }

    // 换到交换链就意味着不再走离屏路径：先把 View 的目标摘掉，否则它还会
    // 往已经废弃的 RenderTarget 上画。离屏资源本身保留着，随时可以切回来。
    if (offscreenBound_)
    {
        view_->setRenderTarget(nullptr);
        offscreenBound_ = false;
    }
    return true;
}

void FilamentRenderer::detachSwapChain()
{
    if (!engine_ || !swapChain_)
        return;

    engine_->flushAndWait();
    engine_->destroy(swapChain_);
    swapChain_ = nullptr;
}

bool FilamentRenderer::attachOffscreen(uint32_t width, uint32_t height)
{
    if (!engine_ || !view_ || width == 0 || height == 0)
        return false;

    // 离屏路径**也要**一条交换链。Filament 的 Renderer::beginFrame 形参带了
    // UTILS_NONNULL（Renderer.h:350），传 nullptr 不是「没有链只画离屏」的意思，
    // 而是直接解引用空指针 —— 曾经这里留空，headless 抓帧在 beginFrame 里
    // SIGSEGV。正确做法是用 Engine.h:865 的重载 createSwapChain(width, height)，
    // 它建一条没有原生 surface 的 headless 链，只为给出一个合法的帧边界。
    if (swapChain_)
    {
        engine_->destroy(swapChain_);
        swapChain_ = nullptr;
    }
    swapChain_ = engine_->createSwapChain(width, height);
    if (!swapChain_)
    {
        std::fprintf(stderr, "[filament] headless createSwapChain 失败 (%ux%u)\n", width, height);
        return false;
    }

    // 重建前先把 View 从旧目标上摘下来 —— 否则销毁纹理时它还挂在 RenderTarget 上。
    view_->setRenderTarget(nullptr);
    if (offscreenRt_)
    {
        engine_->destroy(offscreenRt_);
        offscreenRt_ = nullptr;
    }
    if (offscreenColor_)
    {
        engine_->destroy(offscreenColor_);
        offscreenColor_ = nullptr;
    }
    if (offscreenDepth_)
    {
        engine_->destroy(offscreenDepth_);
        offscreenDepth_ = nullptr;
    }

    using TexUsage = ::filament::backend::TextureUsage;

    // COLOR_ATTACHMENT 供画，BLIT_SRC 是 readPixels 的硬性要求
    // （Renderer.h:579 明写目标纹理必须带 BLIT_SRC，否则运行时断言）。
    // SAMPLEABLE 留着，方便以后把这张图当贴图再喂回场景。
    offscreenColor_ =
        Texture::Builder()
            .width(width)
            .height(height)
            .levels(1)
            .sampler(Texture::Sampler::SAMPLER_2D)
            .usage(TexUsage::COLOR_ATTACHMENT | TexUsage::BLIT_SRC | TexUsage::SAMPLEABLE)
            .format(Texture::InternalFormat::RGBA8)
            .build(*engine_);

    // 深度只要 DEPTH24：没有模板需求，少一档内存也更少一个格式协商失败点。
    offscreenDepth_ = Texture::Builder()
                          .width(width)
                          .height(height)
                          .levels(1)
                          .sampler(Texture::Sampler::SAMPLER_2D)
                          .usage(TexUsage::DEPTH_ATTACHMENT)
                          .format(Texture::InternalFormat::DEPTH24)
                          .build(*engine_);

    if (!offscreenColor_ || !offscreenDepth_)
    {
        std::fprintf(stderr, "[filament] 离屏纹理创建失败 (%ux%u)\n", width, height);
        return false;
    }

    offscreenRt_ = RenderTarget::Builder()
                       .texture(RenderTarget::AttachmentPoint::COLOR0, offscreenColor_)
                       .texture(RenderTarget::AttachmentPoint::DEPTH, offscreenDepth_)
                       .build(*engine_);
    if (!offscreenRt_)
    {
        std::fprintf(stderr, "[filament] RenderTarget 创建失败 (%ux%u)\n", width, height);
        return false;
    }

    view_->setRenderTarget(offscreenRt_);
    view_->setViewport(filament::Viewport{0, 0, width, height});
    offscreenBound_ = true;
    targetWidth_ = width;
    targetHeight_ = height;
    return true;
}

bool FilamentRenderer::resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return false;

    targetWidth_ = width;
    targetHeight_ = height;

    if (offscreenBound_)
        return attachOffscreen(width, height);

    if (view_)
        view_->setViewport(filament::Viewport{0, 0, width, height});
    return true;
}

// =====================================================================
// 帧
// =====================================================================

bool FilamentRenderer::beginFrame(const render::FrameInfo &info)
{
    if (!renderer_ || !view_)
        return false;

    // 没 attach 过就没有合法交换链，而 beginFrame 的形参是 UTILS_NONNULL ——
    // 挡在这里，而不是把空指针喂进去换一个段错误。
    if (!swapChain_)
    {
        std::fprintf(stderr, "[filament] beginFrame：尚未 attach；请先调 attachSwapChain 或 attachOffscreen\n");
        return false;
    }

    ++frameCounter_;

    if (info.world)
    {
        syncWorld(*info.world);
        syncLights(*info.world);
        // 环境要先于渲染设置：applyRenderSettings 会用 indirectLight_ 决定
        // showEnvironment 打开时到底绑不绑 IBL。
        applyEnvironment(info.world->environment());
    }

    if (info.view)
    {
        updateCamera(*info.view);
        applyRenderSettings(info.view->settings);
        if (info.view->viewport.width != 0 && info.view->viewport.height != 0)
            view_->setViewport(filament::Viewport{0, 0, info.view->viewport.width,
                                                  info.view->viewport.height});
    }

    // ClearOptions 由 Renderer::beginFrame 在帧开始时读取。必须先应用本帧的
    // RenderSettings，再 beginFrame；若放在其后，窗口可能继续显示上一帧内容，
    // 动画物体移动后就会留下背景拖影。
    //
    // GPU 落后时 Filament 会要求跳过当前帧。不能在没有可呈现帧的情况下继续
    // render/endFrame；窗口交换链可能保留旧内容，表现为动画运动路径上的黑色拖影。
    // 对外仍返回 true：后端正常工作，只是本次没有提交新帧。
    if (!renderer_->beginFrame(swapChain_))
        return true;

    renderer_->render(view_);
    renderer_->endFrame();
    return true;
}

void FilamentRenderer::endFrame()
{
    // 刻意留空。Filament 的帧边界是 beginFrame / endFrame 把提交夹在中间，
    // 而引擎侧的 IRenderer 把它拆成 beginFrame / endFrame 两个调用；真正
    // 的提交发生在 beginFrame 里（render + endFrame 成对）。这里若再调一次
    // renderer_->endFrame() 就会变成「一帧内两次 endFrame」—— 那是未定义行为，
    // 不是无害的空转。保持幂等的空实现是有意的。
}

bool FilamentRenderer::readbackImage(asset::ImageData &out)
{
    if (!renderer_ || !view_ || !swapChain_ || !offscreenRt_ || !offscreenBound_)
        return false;

    const uint32_t w = targetWidth_;
    const uint32_t h = targetHeight_;
    if (w == 0 || h == 0)
        return false;

    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    void *pixels = std::malloc(bytes);
    if (!pixels)
        return false;
    auto *job = new ReadbackJob();

    // 同 beginFrame：这里是「无论如何要回读一帧」，帧率节流建议不构成失败。
    (void) renderer_->beginFrame(swapChain_);

    renderer_->render(view_);
    renderer_->readPixels(offscreenRt_, 0, 0, w, h,
                          ::filament::backend::PixelBufferDescriptor(
                              pixels, bytes,
                              ::filament::backend::PixelDataFormat::RGBA,
                              ::filament::backend::PixelDataType::UBYTE,
                              &readbackCallback, job));
    renderer_->endFrame();

    // 回调由驱动分发时在主线程调用，flushAndWait 就是驱动这条分发的途径。
    // 循环只是给慢驱动留余量；正常情况下第一轮就返回。
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (job->phase.load(std::memory_order_acquire) == 1)
            break;
        engine_->flushAndWait(50ull * 1000ull * 1000ull);   // 50 ms
    }

    if (job->phase.load(std::memory_order_acquire) != 1)
    {
        // 抢在回调之前把所有权收回自己；抢输了说明数据其实已经到了，
        // 那就掉到成功路径继续走。
        int expected = 0;
        if (job->phase.compare_exchange_strong(expected, 2, std::memory_order_acq_rel))
        {
            // 放弃：从这一刻起不再触碰 job 的任何字段，也不释放任何东西。
            // 回调必然到来并由它 free(buffer) + delete job —— 在这里 delete
            // 会给回调留一个悬垂 job（理由见 readbackCallback 上方）。
            return false;
        }
        // CAS 失败说明回调恰好抢先送达：掉到下面的成功路径。
    }

    // 行序：ImageData 是「第 0 行在顶」，而 Filament 的 readPixels 在 CPU
    // 侧也是从缓冲区低地址开始对应图像顶部（Renderer.h:568 的 y 翻转注释
    // 只对「曾用 setImage 上传过的 OpenGL 纹理」成立；离屏目标是 build()
    // 出来的空纹理，不适用）。所以这里直传，不做翻转。
    out.name = "offscreen";
    out.width = w;
    out.height = h;
    // 关掉后处理意味着 Filament 不做 tonemap / OETF，回读的是**线性**值。
    // 写文件（PNG/PPM）前上层需要自己编码到 sRGB，否则画面会明显偏暗。
    out.colorSpace = asset::ColorSpace::Linear;
    out.pixels.assign(job->buffer, job->buffer + job->size);

    std::free(job->buffer);
    delete job;
    return true;
}

// =====================================================================
// 世界翻译
// =====================================================================

void FilamentRenderer::syncWorld(const scene::World &world)
{
    if (!engine_)
        return;

    TransformManager &tm = engine_->getTransformManager();
    RenderableManager &rm = engine_->getRenderableManager();

    std::unordered_map<uint64_t, PrimitiveRecord> &prims = primitives_;

    // 先假设本帧一个都没见到，之后见到的打上 frameCounter_。收尾时凡不是
    // 本帧的都回收 —— 不需要额外维护「快照 diff」，节点被删的情况自然覆盖。
    for (auto &entry : prims)
        entry.second.lastSeenFrame = 0;

    const std::vector<scene::Node> &nodes = world.nodes();
    uint32_t alive = 0;

    for (size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const scene::Node &node = nodes[ni];
        if (!node.visible)
            continue;

        for (size_t pi = 0; pi < node.primitives.size(); ++pi)
        {
            const scene::Primitive &prim = node.primitives[pi];
            MeshRecord *meshRec = meshes_.get(prim.mesh);
            MaterialRecord *matRec = materials_.get(prim.material);
            if (!meshRec || !meshRec->vb || !meshRec->ib || !matRec || !matRec->instance)
                continue;

            // key 用「节点下标 + 图元下标」合成。它只在 world 的拓扑稳定时
            // 有意义 —— 而 world 拓扑稳定正是这套缓存的适用前提（每帧重建
            // world 的用法下，缓存会退化成每帧全量重建，但仍然正确）。
            const uint64_t key = (static_cast<uint64_t>(ni) << 32) |
                                 static_cast<uint32_t>(pi);

            auto it = prims.find(key);
            const bool needsRebuild =
                it == prims.end() || it->second.mesh != prim.mesh ||
                it->second.material != prim.material ||
                it->second.indexOffset != prim.indexOffset ||
                it->second.indexCount != prim.indexCount;

            if (needsRebuild)
            {
                if (it != prims.end())
                {
                    destroyEntity(it->second.entity);
                    prims.erase(it);
                }

                PrimitiveRecord rec;
                rec.entity = utils::EntityManager::get().create();
                rec.mesh = prim.mesh;
                rec.material = prim.material;
                rec.indexOffset = prim.indexOffset;
                rec.indexCount = prim.indexCount;
                rec.hasTransformComponent = false;

                // indexCount == 0 是引擎侧的约定：用整个网格（见 world.hpp）。
                const uint32_t indexCount =
                    prim.indexCount != 0 ? prim.indexCount : meshRec->indexCount;

                const math::AABB &bounds =
                    meshRec->bounds.valid ? meshRec->bounds : node.meshBounds;

                RenderableManager::Builder builder(1);
                builder.geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                                 meshRec->vb, meshRec->ib,
                                 static_cast<uint32_t>(prim.indexOffset), indexCount)
                    .material(0, matRec->instance)
                    .boundingBox(toFilamentBox(bounds))
                    // 动画每帧只更新当前姿势的 AABB；Filament 要求蒙皮网格的包围盒
                    // 覆盖动画所有可能位置。没有离线全动画包围盒时不能安全剔除，
                    // 否则肢体移动到当前 AABB 外就会闪烁/消失。
                    .culling(!meshRec->skinned);

                if (builder.build(*engine_, rec.entity) !=
                    RenderableManager::Builder::Success)
                {
                    std::fprintf(stderr,
                                 "[filament] RenderableManager::build 失败（node %zu, prim %zu）\n",
                                 ni, pi);
                    destroyEntity(rec.entity);
                    continue;
                }

                // TransformManager 的组件必须显式创建，且每个实体只能创建一次。
                tm.create(rec.entity);
                rec.hasTransformComponent = true;
                rec.lastMatrix = node.worldMatrix;
                tm.setTransform(tm.getInstance(rec.entity), toFilamentMat4(node.worldMatrix));

                // 关键一步：实体必须显式登记进 Scene，否则它只是一个「组件齐全
                // 但没人渲染」的游离实体 —— 画面会安静地只剩清屏色。
                scene_->addEntity(rec.entity);

                it = prims.emplace(key, rec).first;
            }

            PrimitiveRecord &rec = it->second;
            rec.lastSeenFrame = frameCounter_;

            // 变换只在真变了才写回。world.updateTransforms() 由调用方负责，
            // 这里只读 worldMatrix；静止场景下这一支永不触发。
            if (rec.hasTransformComponent && !sameMatrix(rec.lastMatrix, node.worldMatrix))
            {
                rec.lastMatrix = node.worldMatrix;
                tm.setTransform(tm.getInstance(rec.entity), toFilamentMat4(node.worldMatrix));
            }

            ++alive;
        }
    }

    // 回收本帧没出现的图元。
    for (auto it = prims.begin(); it != prims.end();)
    {
        if (it->second.lastSeenFrame != frameCounter_)
        {
            destroyEntity(it->second.entity);
            it = prims.erase(it);
        }
        else
        {
            ++it;
        }
    }

    (void)rm;   // 材质绑定走 Builder，rm 只用于将来可能的 readback 查询
    lastFramePrimitives_ = alive;
}

void FilamentRenderer::syncLights(const scene::World &world)
{
    if (!engine_)
        return;

    LightManager &lm = engine_->getLightManager();
    const std::vector<scene::LightDesc> &lights = world.lights();

    // 数量先对齐：多出来的销毁，少了的补实体。灯光没有稳定 id，只能按
    // 位置对应 —— 增删中间的灯会连带重建其后的所有灯。对「灯光少且稳定」
    // 的典型场景这是零成本；对每帧乱序增删的用法是正确但低效的选择。
    while (lightEntities_.size() > lights.size())
    {
        destroyEntity(lightEntities_.back());
        lightEntities_.pop_back();
    }
    while (lightEntities_.size() < lights.size())
        lightEntities_.push_back(utils::EntityManager::get().create());

    for (size_t i = 0; i < lights.size(); ++i)
    {
        const scene::LightDesc &src = lights[i];
        const utils::Entity entity = lightEntities_[i];

        const bool directional = src.type == scene::LightType::Directional;
        const LightManager::Type type = directional ? LightManager::Type::DIRECTIONAL
                                                    : LightManager::Type::POINT;

        filament::math::float3 dir = toFilamentVec3(src.direction);
        // filament::math 没有自由函数 dot/normalize（它们在 details 命名空间里，
        // 不是公开接口），所以这里直接手算。顺便也避开了「到底该用哪一层 math」
        // 这个问题 —— 两行算术比跨命名空间的依赖更便宜。
        const float len2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
        if (len2 < 1.0e-12f)
        {
            dir = filament::math::float3{0.0f, -1.0f, 0.0f};   // 退化方向兜底
        }
        else
        {
            const float inv = 1.0f / std::sqrt(len2);
            dir = filament::math::float3{dir.x * inv, dir.y * inv, dir.z * inv};
        }

        const filament::math::float3 color{src.color.x, src.color.y, src.color.z};

        // Builder::build 会替换实体上已有的 Light 组件（文档明写），所以不需要
        // 先 hasComponent/destroy —— 重复 build 是合法且幂等的语义。
        LightManager::Builder builder(type);
        builder.color(LinearColor{color})
            .castShadows(src.castShadows)
            .intensity(directional ? src.intensity * kLuxPerIntensityUnit
                                   : src.intensity * kLumenPerIntensityUnit);

        if (directional)
        {
            // LightDesc::direction 的语义是「光传播方向」（从光源射向场景），
            // 与 Filament Builder::direction() 同义 —— 不要取反。
            builder.direction(dir);
        }
        else
        {
            builder.position(toFilamentVec3(src.position));
            // range <= 0 表示无限远，对应 Filament 的 falloff = 0（不衰减）。
            if (src.range > 0.0f)
                builder.falloff(std::max(src.range, 1.0e-3f));
        }

        if (builder.build(*engine_, entity) == LightManager::Builder::Success)
        {
            // 和 Renderable 同理：灯光也要登记进 Scene 才会参与着色。
            scene_->addEntity(entity);
        }
        else
        {
            std::fprintf(stderr, "[filament] LightManager::build 失败（light %zu）\n", i);
        }
    }

    (void)lm;
}

void FilamentRenderer::updateCamera(const render::View &view)
{
    if (!camera_)
        return;

    const scene::Camera &cam = view.camera;

    const double aspect = (view.viewport.height != 0)
                              ? static_cast<double>(view.viewport.width) /
                                    static_cast<double>(view.viewport.height)
                              : static_cast<double>(cam.aspect);

    // fovYRadians 是**弧度**，Filament 收的是**度**。
    //
    // 这里刻意不用 cam.projection()（即 math::perspective）—— 那是 GL 约定
    // 的 NDC（z ∈ [-1, 1]），而 Filament 用 reversed-Z 无限远投影，两者
    // 不通用。投影必须交给 Filament 自己算，只把 fov/near/far 传进去。
    camera_->setProjection(math::degrees(cam.fovYRadians), aspect,
                           static_cast<double>(cam.nearPlane),
                           static_cast<double>(cam.farPlane),
                           ::filament::Camera::Fov::VERTICAL);

    // 相机模型矩阵 = 视图矩阵的逆。math::Mat4 与 filament::math::mat4f 同为
    // 列主序，toFilamentMat4 直接 memcpy，不需要转置。
    camera_->setModelMatrix(toFilamentMat4(math::inverse(cam.view())));

    camera_->setExposure(view.settings.exposure);
}

void FilamentRenderer::applyRenderSettings(const render::RenderSettings &settings)
{
    if (!renderer_ || !scene_ || !camera_)
        return;

    // ClearOptions::clear 的默认值是 **false**。不显式置 true 的话离屏目标
    // 会保留上一帧内容（在交换链上就是闪烁的残影），而且只靠 clearColor
    // 传进去完全看不出问题 —— 这是最容易漏的一行。
    Renderer::ClearOptions clearOptions;
    clearOptions.clearColor = {settings.clearColor.x, settings.clearColor.y,
                               settings.clearColor.z, 1.0f};
    clearOptions.clear = true;
    clearOptions.discard = true;
    renderer_->setClearOptions(clearOptions);

    // 环境：关掉时天空盒与 IBL 一起摘。只摘天空盒会留下反射照明，
    // 画面看着像「关了一半」，是最容易让人误判成 bug 的中间态。
    scene_->setSkybox(settings.showEnvironment ? skybox_ : nullptr);
    scene_->setIndirectLight(settings.showEnvironment ? indirectLight_ : nullptr);
    showEnvironment_ = settings.showEnvironment;

    // 剔除：Filament 的 View 没有全局剔除开关，但 MaterialInstance 有
    // CullingMode —— 逐个实例改是唯一的真实落点（去改材质包本身会污染
    // 所有共享该材质的实例）。
    materials_.forEach([&settings](const MaterialRecord &rec)
                       {
                           if (rec.instance)
                               rec.instance->setCullingMode(
                                   settings.enableCulling && !rec.doubleSided
                                       ? ::filament::backend::CullingMode::BACK
                                       : ::filament::backend::CullingMode::NONE);
                       });

    // wireframe / enableDepthTest 在 Filament 的 View 上没有等价开关：
    // 线框不是运行时状态（Filament 走材质变体），深度测试由管线状态决定，
    // 都不对 View 开放。**不假装实现** —— 只提醒一次，避免每帧刷屏。
    if (settings.wireframe && !warnedWireframe_)
    {
        warnedWireframe_ = true;
        std::fprintf(stderr,
                     "[filament] RenderSettings::wireframe 无对应实现"
                     "（Filament View 没有运行时线框状态，需要材质变体）\n");
    }
    if (!settings.enableDepthTest && !warnedDepthTest_)
    {
        warnedDepthTest_ = true;
        std::fprintf(stderr,
                     "[filament] RenderSettings::enableDepthTest=false 无对应实现"
                     "（深度测试由 Filament 管线状态固定）\n");
    }
}

// =====================================================================
// 环境 / IBL
// =====================================================================

bool FilamentRenderer::applyEnvironment(const scene::Environment &env)
{
    if (!engine_ || !scene_)
        return false;

    IndirectLight *ibl = nullptr;
    IblRecord *rec = env.ibl.valid() ? ibls_.get(env.ibl) : nullptr;
    if (rec)
        ibl = rec->indirectLight;

    if (ibl)
    {
        // 引擎侧的 iblIntensity 是相对倍率（1.0 = 标准环境），Filament 的
        // setIntensity 收物理量（lux）。只做一次线性映射，1.0 就有可用亮度。
        ibl->setIntensity(env.iblIntensity * kIblIntensityUnit);

        // SH 球谐：IndirectLight 没有 SH setter，只能通过 Builder 重设，
        // 所以缓存一份已应用的值，只有真变了才重建 —— 否则每帧重建一个
        // IndirectLight 是纯浪费。
        const bool bandsChanged =
            !hasAppliedSh_ || !sameBands(appliedShBands_, env.shBands);
        if (anyNonZero(env.shBands) && bandsChanged && rec && rec->reflections)
        {
            filament::math::float3 bands[9];
            for (int i = 0; i < 9; ++i)
                bands[i] = toFilamentVec3(env.shBands[i]);

            // 注意第二个参数是**球谐阶数**不是数组长度：上游
            // IndirectLight.h:154 要求 1/2/3，数组大小分别是 1/4/9。
            // 传 9 会被当作「9 阶」直接拒绝。
            IndirectLight *rebuilt = IndirectLight::Builder()
                                         .reflections(rec->reflections)
                                         .irradiance(3, bands)
                                         .intensity(env.iblIntensity * kIblIntensityUnit)
                                         .build(*engine_);
            if (rebuilt)
            {
                if (rec->indirectLight)
                    engine_->destroy(rec->indirectLight);
                rec->indirectLight = rebuilt;
                ibl = rebuilt;
            }
        }
        if (bandsChanged)
        {
            for (int i = 0; i < 9; ++i)
                appliedShBands_[i] = env.shBands[i];
            hasAppliedSh_ = true;
        }
    }

    indirectLight_ = ibl;
    currentIbl_ = env.ibl;

    // ambientColor / reflectionColor 没有对应的 Filament 概念：前者是
    // 环境色的**常量兜底**（Filament 的环境光只能来自 IBL 或灯），后者
    // 会与 cubemap 的反射色调冲突。两者在这里被忽略是**已知的语义损失**，
    // 不是遗漏 —— 需要它们在 GPU 侧也生效的话，得先给 Environment 加一个
    // 常量环境项，并让 Filament 侧用 AmbientLight 或补一张 1x1 IBL 来表达。
    return ibl != nullptr;
}

asset::ImagePtr FilamentRenderer::packAormImage(const asset::MaterialDesc &desc)
{
    const bool hasAo = !desc.ambientOcclusion.empty();
    const bool hasRough = !desc.roughness.empty();
    const bool hasMetal = !desc.metallic.empty();
    if (!hasAo && !hasRough && !hasMetal)
        return nullptr;   // 三张全缺 -> 交给 neutralTexture，别白造一张

    // 尺寸取第一张存在的贴图；其余按最近邻缩放到同一尺寸。
    // 采样器本身不擅长逐纹理缩放，而 AORM 打包必须三通道同尺寸。
    const asset::ImageData *base = hasAo && desc.ambientOcclusion.image
                                       ? desc.ambientOcclusion.image.get()
                                   : hasRough && desc.roughness.image
                                       ? desc.roughness.image.get()
                                   : desc.metallic.image.get();
    if (!base || !base->valid())
        return nullptr;

    const uint32_t w = base->width;
    const uint32_t h = base->height;

    auto out = std::make_shared<asset::ImageData>();
    out->name = desc.name + ":aorm";
    out->width = w;
    out->height = h;
    // AO / Roughness / Metallic 都是线性数据。
    out->colorSpace = asset::ColorSpace::Linear;
    out->pixels.resize(static_cast<size_t>(w) * h * 4u);

    const auto fetch = [w, h](const asset::TextureSlot &slot, uint32_t x, uint32_t y) -> float
    {
        if (slot.empty() || !slot.image || !slot.image->valid())
            return 1.0f;   // 缺哪张就补「无作用」的值（1.0 对三者都是恒等元）
        const asset::ImageData &img = *slot.image;
        const uint32_t sx = static_cast<uint32_t>(
            (static_cast<uint64_t>(x) * img.width) / (w ? w : 1u));
        const uint32_t sy = static_cast<uint32_t>(
            (static_cast<uint64_t>(y) * img.height) / (h ? h : 1u));
        return static_cast<float>(img.texel(sx < img.width ? sx : img.width - 1u,
                                            sy < img.height ? sy : img.height - 1u)[0]) /
               255.0f;
    };

    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t *dst = out->pixels.data() + (static_cast<size_t>(y) * w + x) * 4u;
            dst[0] = static_cast<uint8_t>(fetch(desc.ambientOcclusion, x, y) * 255.0f + 0.5f);
            dst[1] = static_cast<uint8_t>(fetch(desc.roughness, x, y) * 255.0f + 0.5f);
            dst[2] = static_cast<uint8_t>(fetch(desc.metallic, x, y) * 255.0f + 0.5f);
            dst[3] = 255;
        }
    }
    return out;
}

IblHandle FilamentRenderer::loadIblFromKtx(const std::string &relativePath)
{
    if (!engine_)
        return {};

    const std::string path = resolvePath(relativePath);

    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes))
    {
        std::fprintf(stderr, "[filament] IBL 读取失败: %s\n", path.c_str());
        return {};
    }

    Texture *reflections = nullptr;
    if (endsWith(path, ".ktx2"))
    {
        // 先验魔数，避免把非 KTX2 字节喂进解码器。
        if (hasIdentifier(bytes, kKtx2Identifier))
            // IBL 反射贴图按线性语义要；文件若自报 sRGB 则由 helper 自动回退。
            reflections = loadKtx2Texture(*engine_, bytes,
                                          ktxreader::Ktx2Reader::TransferFunction::LINEAR);
    }
    else
    {
        // 字节的生命周期交给 createKtx1Texture —— 见 Ktx1Pack 的说明。
        reflections = createKtx1Texture(engine_, std::move(bytes), /*srgb=*/false);
    }

    if (!reflections)
    {
        std::fprintf(stderr, "[filament] IBL 解码失败: %s\n", path.c_str());
        return {};
    }

    IndirectLight *indirectLight = IndirectLight::Builder()
                                       .reflections(reflections)
                                       .intensity(kIblIntensityUnit)
                                       .build(*engine_);
    if (!indirectLight)
    {
        engine_->destroy(reflections);
        std::fprintf(stderr, "[filament] IndirectLight 创建失败: %s\n", path.c_str());
        return {};
    }

    IblRecord rec;
    rec.indirectLight = indirectLight;
    rec.reflections = reflections;
    rec.ownsReflections = true;
    return ibls_.insert(rec);
}

bool FilamentRenderer::setSkyboxFromKtx(const std::string &relativePath)
{
    if (!engine_ || !scene_)
        return false;

    const std::string path = resolvePath(relativePath);

    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes))
    {
        std::fprintf(stderr, "[filament] 天空盒读取失败: %s\n", path.c_str());
        return false;
    }

    Texture *environment = nullptr;
    if (endsWith(path, ".ktx2"))
    {
        if (hasIdentifier(bytes, kKtx2Identifier))
            // 天空盒本该是 sRGB；文件若自报线性则由 helper 自动回退。
            environment = loadKtx2Texture(*engine_, bytes,
                                          ktxreader::Ktx2Reader::TransferFunction::sRGB);
    }
    else
    {
        // 字节的生命周期交给 createKtx1Texture —— 见 Ktx1Pack 的说明。
        environment = createKtx1Texture(engine_, std::move(bytes), /*srgb=*/true);
    }

    if (!environment)
    {
        std::fprintf(stderr, "[filament] 天空盒解码失败: %s\n", path.c_str());
        return false;
    }

    Skybox *skybox = Skybox::Builder()
                         // environment() 的参数是**非 const** 的 Texture*，
                         // 这里是 Filament 签名如此，不是笔误。
                         .environment(environment)
                         .showSun(false)
                         .intensity(1.0f)
                         .build(*engine_);
    if (!skybox)
    {
        engine_->destroy(environment);
        return false;
    }

    // 换天空盒时销毁旧的：Skybox 对象自身不持有纹理（纹理由我们管），
    // 所以要先记住旧纹理再销毁 Skybox，否则就漏了。
    if (skybox_)
    {
        engine_->destroy(skybox_);
        skybox_ = nullptr;
    }
    if (skyboxEnvironment_)
    {
        // 注意：若它正被旧 skybox_ 引用，必须先销毁 skybox_（上面已做）。
        engine_->destroy(skyboxEnvironment_);
        skyboxEnvironment_ = nullptr;
    }

    skybox_ = skybox;
    skyboxEnvironment_ = environment;
    if (showEnvironment_)
        scene_->setSkybox(skybox_);
    return true;
}

void FilamentRenderer::destroy(IblHandle handle)
{
    IblRecord *rec = ibls_.get(handle);
    if (!rec)
        return;

    // 解绑：Scene 还指着它的话，先摘掉再销毁，避免留下悬垂指针。
    if (indirectLight_ == rec->indirectLight)
    {
        if (scene_)
            scene_->setIndirectLight(nullptr);
        indirectLight_ = nullptr;
    }

    if (rec->indirectLight)
        engine_->destroy(rec->indirectLight);
    if (rec->reflections && rec->ownsReflections)
        engine_->destroy(rec->reflections);

    ibls_.erase(handle);
}

} // namespace my3d::backend::filament
