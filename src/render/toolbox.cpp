#include "render/toolbox.hpp"

#include <algorithm>

namespace my3d::render
{

void buildDrawItems(const scene::World &world, std::vector<DrawItem> &out)
{
    out.clear();

    const std::vector<scene::Node> &nodes = world.nodes();

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const scene::Node &n = nodes[i];
        if (!n.visible || n.primitives.empty())
            continue;

        for (const scene::Primitive &p : n.primitives)
        {
            DrawItem item;
            item.mesh = p.mesh;
            item.indexOffset = p.indexOffset;
            item.indexCount = p.indexCount;
            item.material = p.material;
            item.worldTransform = n.worldMatrix;
            item.worldBounds = n.worldBounds;
            out.push_back(item);
        }
    }
}

void cullByFrustum(const math::Frustum &frustum, std::vector<DrawItem> &items)
{
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&frustum](const DrawItem &item)
                               {
                                   if (!item.worldBounds.valid)
                                       return false;   // 无包围盒 -> 保守保留
                                   return !frustum.intersects(item.worldBounds);
                               }),
                items.end());
}

void sortDrawItems(std::vector<DrawItem> &items, SortMode mode,
                   const math::Vec3 &cameraPosition)
{
    switch (mode)
    {
    case SortMode::None:
        return;

    case SortMode::FrontToBack:
        std::stable_sort(items.begin(), items.end(),
                         [&cameraPosition](const DrawItem &a, const DrawItem &b)
                         {
                             return math::lengthSq(a.worldBounds.center() - cameraPosition) <
                                    math::lengthSq(b.worldBounds.center() - cameraPosition);
                         });
        return;

    case SortMode::BackToFront:
        std::stable_sort(items.begin(), items.end(),
                         [&cameraPosition](const DrawItem &a, const DrawItem &b)
                         {
                             return math::lengthSq(a.worldBounds.center() - cameraPosition) >
                                    math::lengthSq(b.worldBounds.center() - cameraPosition);
                         });
        return;

    case SortMode::ByMaterial:
        std::stable_sort(items.begin(), items.end(),
                         [](const DrawItem &a, const DrawItem &b)
                         { return a.material.index < b.material.index; });
        return;
    }
}

math::Vec3 evalDiffuse(const scene::LightDesc &light, const math::Vec3 &N,
                       const math::Vec3 &albedo, const math::Vec3 &surfacePoint)
{
    const math::Vec3 L = light.toLight(surfacePoint);
    const float ndotl = math::dot(N, L);
    if (ndotl <= 0.0f)
        return {0.0f, 0.0f, 0.0f};

    float atten = 1.0f;
    if (light.type == scene::LightType::Point)
        atten = light.attenuation(math::length(light.position - surfacePoint));

    const float k = ndotl * light.intensity * atten;
    return {albedo.x * light.color.x * k,
            albedo.y * light.color.y * k,
            albedo.z * light.color.z * k};
}

math::Vec3 evalDirectional(const scene::LightDesc &light, const math::Vec3 &N,
                           const math::Vec3 &albedo)
{
    // 方向光与表面点无关，传原点即可（toLight 会走 Directional 分支）。
    return evalDiffuse(light, N, albedo, math::Vec3{0.0f, 0.0f, 0.0f});
}

math::Vec3 evalIrradianceSH(const math::Vec3 sh[9], const math::Vec3 &normal)
{
    // 标准三阶球谐基（Ramamoorthi & Hanrahan 2001），
    // 以及 Lambert 卷积系数 A_l：A0 = 1, A1 = 2/3, A2 = 1/4（已含 1/π 归一）。
    const float x = normal.x;
    const float y = normal.y;
    const float z = normal.z;

    const float basis[9] = {
        0.282095f,
        0.488603f * y,
        0.488603f * z,
        0.488603f * x,
        1.092548f * x * y,
        1.092548f * y * z,
        0.315392f * (3.0f * z * z - 1.0f),
        1.092548f * x * z,
        0.546274f * (x * x - y * y),
    };

    const float conv[9] = {
        1.0f,
        2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f,
        1.0f / 4.0f, 1.0f / 4.0f, 1.0f / 4.0f, 1.0f / 4.0f, 1.0f / 4.0f,
    };

    math::Vec3 out{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 9; ++i)
        out = out + sh[i] * (basis[i] * conv[i]);
    return out;
}

} // namespace my3d::render
