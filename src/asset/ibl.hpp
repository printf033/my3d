#pragma once

// 基于图像的光照（IBL）。
//
// 这里只承载**漫反射辐照度** —— 9 个球谐系数，纯数据，CPU / ASCII 后端直接
// 参与着色。镜面反射环境（KTX 立方体贴图）不经 asset 层：路径由上层持有，
// 直接交给图形后端（`FilamentRenderer::loadIblFromKtx`）；CPU / ASCII 后端不做
// 这项降级，在文档 §6.6 里显式声明。

#include "core/math.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace my3d::asset
{

struct SphericalHarmonics
{
    std::array<math::Vec3, 9> bands{};
    bool valid = false;
};

// 读取 cmgen 生成的 sh.txt（9 行 `(r, g, b); // comment`）。
// 解析失败时 out.valid == false 并返回 false —— 不写日志、不抛异常，由调用者决定。
bool loadSphericalHarmonics(const std::string &path, SphericalHarmonics &out) noexcept;

} // namespace my3d::asset
