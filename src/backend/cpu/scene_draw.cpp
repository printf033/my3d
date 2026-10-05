#include "backend/cpu/scene_draw.hpp"

#include "core/math.hpp"
#include "render/toolbox.hpp"

#include <algorithm>
#include <iterator>

namespace my3d::backend::cpu
{
namespace
{

// 缺材质时的兜底色。
//
// 用中性灰白而不是黑：忘记绑材质时，黑色会让物体「凭空消失」，看起来像
// 剔除 bug 或变换错误；灰白则明确表现为「没上色」，一眼能定位到材质。
constexpr math::Vec3 kDefaultAlbedo{0.8f, 0.8f, 0.8f};

inline math::Vec3 rgb(const math::Vec4 &v) noexcept { return {v.x, v.y, v.z}; }

math::Vec4 sampleTexture(const asset::TextureSlot &slot, const math::Vec2 &uv) noexcept
{
    if (!slot.image || !slot.image->valid())
        return {1.0f, 1.0f, 1.0f, 1.0f};

    uint8_t pixel[4];
    slot.image->fetchRGBA(uv.x, uv.y, pixel);
    const float r = static_cast<float>(pixel[0]) / 255.0f;
    const float g = static_cast<float>(pixel[1]) / 255.0f;
    const float b = static_cast<float>(pixel[2]) / 255.0f;
    const float a = static_cast<float>(pixel[3]) / 255.0f;
    if (slot.image->colorSpace == asset::ColorSpace::SRGB)
        return {asset::srgbToLinear(r), asset::srgbToLinear(g), asset::srgbToLinear(b), a};
    return {r, g, b, a};
}

} // namespace

bool drawScene(const render::FrameInfo &info, const ResourceView &resources, Framebuffer &target,
               RasterStats &stats, SceneDrawStats &sceneStats, std::vector<render::DrawItem> &scratch)
{
    stats = RasterStats{};
    sceneStats = SceneDrawStats{};

    if (info.world == nullptr || info.view == nullptr)
        return false;

    const render::View &view = *info.view;
    const uint32_t width = view.viewport.width;
    const uint32_t height = view.viewport.height;
    if (width == 0 || height == 0)
        return false;

    target.resize(width, height);
    target.clear(view.settings.clearColor);

    Lighting lighting = Lighting::from(*info.world);
    if (!view.settings.showEnvironment)
    {
        // showEnvironment == false 表示「只要直接光」。
        // 这里清的是 Lighting 快照而不是 World —— World 是共享的只读输入，
        // 为了一个视图的设置去改它就污染了别的东西。
        std::fill(std::begin(lighting.sh), std::end(lighting.sh), math::Vec3{});
        lighting.hasSH = false;
        lighting.ambient = math::Vec3{};
    }

    scratch.clear();
    render::buildDrawItems(*info.world, scratch);
    sceneStats.drawItems = static_cast<uint32_t>(scratch.size());

    if (view.settings.enableCulling)
    {
        const math::Frustum frustum = view.camera.frustum();
        scratch.erase(std::remove_if(scratch.begin(), scratch.end(),
                                     [&frustum, &resources](const render::DrawItem &item)
                                     {
                                         const asset::MeshData *mesh = resources.mesh(item.mesh);
                                         // 蒙皮顶点每帧改变，但 World 中的 meshBounds 仍是
                                         // 绑定姿态的范围。用它剔除动画网格会在近距离等
                                         // 姿势变化时误删整块网格；没有动画全程范围时，
                                         // 保守保留蒙皮网格，静态网格仍正常剔除。
                                         if (mesh != nullptr && mesh->skinned())
                                             return false;
                                         if (!item.worldBounds.valid)
                                             return false;
                                         return !frustum.intersects(item.worldBounds);
                                     }),
                      scratch.end());
        sceneStats.drawItemsCulled = sceneStats.drawItems - static_cast<uint32_t>(scratch.size());
    }

    // 不透明几何体由 z-buffer 保证正确性，排序只影响 overdraw 与混合顺序。
    // FrontToBack 让近处的面先写深度，远处的片段在深度测试就被丢掉。
    render::sortDrawItems(scratch, render::SortMode::FrontToBack, view.camera.position);

    const math::Mat4 viewProjection = view.camera.viewProjection();
    const bool wireframe = view.settings.wireframe;
    const float exposure = view.settings.exposure;

    // 目标 Framebuffer 是共享的，裁剪在它自己的坐标系里进行。
    for (const render::DrawItem &item : scratch)
    {
        const asset::MeshData *mesh = resources.mesh(item.mesh);
        if (mesh == nullptr || mesh->vertices.empty())
            continue;

        const uint32_t totalIndices = mesh->indexCount();
        const uint32_t first = item.indexOffset;
        if (first >= totalIndices)
            continue;

        // indexCount == 0 是「用整个网格」的约定（见 scene::Primitive）。
        // 但 indexOffset 非零时，「整个」只能是剩余部分，不能是全部 ——
        // 否则一个网格被切成多段时，每段都会从头画一遍完整网格。
        uint32_t count = item.indexCount != 0 ? item.indexCount : totalIndices - first;
        if (first + count > totalIndices)
            count = totalIndices - first;
        count -= count % 3u; // 只处理完整三角形
        if (count == 0)
            continue;

        const asset::MaterialDesc *material = resources.material(item.material);
        const math::Vec4 baseColorFactor =
            material != nullptr ? material->baseColorFactor
                                : math::Vec4{kDefaultAlbedo.x, kDefaultAlbedo.y, kDefaultAlbedo.z, 1.0f};
        const asset::TextureSlot *baseColorSlot =
            material != nullptr ? &material->baseColor : nullptr;
        const asset::TextureSlot *emissiveSlot =
            material != nullptr ? &material->emissive : nullptr;
        // emissiveFactor 是**线性**附加项，不参与光照。
        const math::Vec3 emissive =
            material != nullptr ? rgb(material->emissiveFactor) : math::Vec3{};
        const bool alphaMask = material != nullptr && material->alphaMask;
        const float alphaCutoff = material != nullptr ? material->alphaCutoff : 0.5f;
        // 双面材质必须关掉背面剔除。注意这只影响剔除，不影响法线朝向 ——
        // 背面片元的法线仍然背对相机，着色会是暗的。真双面需要翻转法线，
        // 那是材质求值的事，这里不擅自替调用方决定。
        const bool doubleSided = material != nullptr && material->doubleSided;

        const math::Mat4 modelViewProjection = viewProjection * item.worldTransform;
        const math::Mat3 normalMat = math::normalMatrix(item.worldTransform);

        RasterSettings rasterSettings;
        rasterSettings.cullBackFaces = view.settings.enableCulling && !doubleSided;
        rasterSettings.depthTest = view.settings.enableDepthTest;
        // 关掉深度测试时也不写深度：否则后续几何体会被一个「只读不测」的
        // 深度缓冲挡住，画面比完全不测试更难解释。
        rasterSettings.depthWrite = view.settings.enableDepthTest;

        const auto shader = [&lighting, baseColorFactor, baseColorSlot, emissive,
                             emissiveSlot, alphaMask, alphaCutoff, exposure](const Fragment &f) {
            const math::Vec4 baseSample =
                baseColorSlot != nullptr ? sampleTexture(*baseColorSlot, f.uv)
                                         : math::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            const math::Vec3 albedo{baseColorFactor.x * baseSample.x,
                                    baseColorFactor.y * baseSample.y,
                                    baseColorFactor.z * baseSample.z};
            if (alphaMask && baseColorFactor.w * baseSample.w < alphaCutoff)
                return FragmentOutput{{}, true};

            const math::Vec3 lit = shade(lighting, f.worldPosition, f.worldNormal, albedo);
            math::Vec3 emission = emissive;
            if (emissiveSlot != nullptr)
            {
                const math::Vec4 emissiveSample = sampleTexture(*emissiveSlot, f.uv);
                emission = {emission.x * emissiveSample.x,
                            emission.y * emissiveSample.y,
                            emission.z * emissiveSample.z};
            }
            return FragmentOutput{tonemap(lit + emission, exposure), false};
        };

        const auto vertexAt = [&mesh, &modelViewProjection, &normalMat, &item](uint32_t index) {
            const asset::Vertex &v = mesh->vertices[index];
            const math::Vec4 worldPosition = math::transformPoint(item.worldTransform, v.position);

            RasterVertex out;
            out.clip = math::transformPoint(modelViewProjection, v.position);
            out.worldPosition = math::Vec3{worldPosition.x, worldPosition.y, worldPosition.z};
            out.worldNormal = math::normalize(math::transformNormal(normalMat, v.normal));
            out.uv = v.uv0;
            return out;
        };

        ++sceneStats.drawCalls;
        sceneStats.triangles += count / 3u;

        for (uint32_t i = 0; i < count; i += 3u)
        {
            const RasterVertex a = vertexAt(mesh->indices[first + i]);
            const RasterVertex b = vertexAt(mesh->indices[first + i + 1]);
            const RasterVertex c = vertexAt(mesh->indices[first + i + 2]);

            if (wireframe)
            {
                rasterizeLine(target, a, b, shader, rasterSettings, stats);
                rasterizeLine(target, b, c, shader, rasterSettings, stats);
                rasterizeLine(target, c, a, shader, rasterSettings, stats);
            }
            else
            {
                rasterizeTriangle(target, a, b, c, shader, rasterSettings, stats);
            }
        }
    }

    return true;
}

} // namespace my3d::backend::cpu
