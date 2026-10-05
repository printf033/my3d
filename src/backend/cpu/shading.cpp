#include "backend/cpu/shading.hpp"

#include "render/toolbox.hpp"

#include <algorithm>

namespace my3d::backend::cpu
{

Lighting Lighting::from(const scene::World &world)
{
    Lighting lighting;

    lighting.lights = world.lights();

    const scene::Environment &env = world.environment();
    lighting.ambient = env.ambientColor;

    // shBands 全零是「未设置」的约定（见 scene::Environment 的注释），
    // 不是「环境光是黑的」。要区分这两种情况：把零系数喂进 SH 求值会得到
    // 一个恒为零的辐照度，背光面会完全黑掉；而用 ambientColor 兜底
    // 至少还看得见结构。
    bool anyBand = false;
    for (int i = 0; i < 9; ++i)
    {
        lighting.sh[i] = env.shBands[i];
        if (math::lengthSq(env.shBands[i]) > 0.0f)
            anyBand = true;
    }
    lighting.hasSH = anyBand;

    return lighting;
}

math::Vec3 shade(const Lighting &lighting, const math::Vec3 &surfacePoint,
                 const math::Vec3 &normal, const math::Vec3 &albedo)
{
    // 间接光：有球谐就按法线方向求辐照度，否则退回常量环境项。
    const math::Vec3 ambient = lighting.hasSH
                                   ? render::evalIrradianceSH(lighting.sh, normal)
                                   : lighting.ambient;

    math::Vec3 color{albedo.x * ambient.x, albedo.y * ambient.y, albedo.z * ambient.z};

    // 直接光。注意 evalDiffuse 内部已经把 albedo 算进去了，
    // 这里不要再乘一次 —— 重复乘会让亮部迅速过曝。
    for (const scene::LightDesc &light : lighting.lights)
        color += render::evalDiffuse(light, normal, albedo, surfacePoint);

    return color;
}

math::Vec3 tonemap(const math::Vec3 &linear, float exposure) noexcept
{
    const auto curve = [exposure](float x) {
        // HDR 里不该出现负值，但法线/光照的数值噪声偶尔会挤出一个极小的负数，
        // 直接进曲线会得到负亮度，转 8 位时回绕成刺眼的白点。
        const float exposed = std::max(x, 0.0f) * exposure;
        return exposed / (1.0f + exposed);
    };

    return math::Vec3{curve(linear.x), curve(linear.y), curve(linear.z)};
}

} // namespace my3d::backend::cpu
