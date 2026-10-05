#pragma once

// my3d::render —— 帧与视图的通用类型。
//
// 依赖方向：render -> scene -> core，绝不反向。后端实现位于 backend/，
// 依赖 render；render 自己不认识任何后端。

#include "scene/camera.hpp"
#include "scene/world.hpp"

namespace my3d::render
{

struct Viewport
{
    uint32_t width = 0;
    uint32_t height = 0;
    float aspect() const noexcept
    {
        return height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    }
};

// 后端渲染设置。
//
// 刻意保持很小，只放「三种后端都能有意义地响应」的项。ASCII 后端的字符集、
// 抖动、色深等属于该后端自己的配置结构（backend/ascii），不往这里塞 ——
// 通用设置被某一种后端的私货污染，是抽象腐化的常见起点。
struct RenderSettings
{
    bool wireframe = false;
    bool showEnvironment = true;
    bool enableCulling = true;
    bool enableDepthTest = true;
    float exposure = 1.0f;
    math::Vec3 clearColor{0.05f, 0.05f, 0.07f};
};

// 一次绘制所看到的东西：相机 + 视口 + 设置。三种后端都消费它。
struct View
{
    scene::Camera camera;
    Viewport viewport;
    RenderSettings settings;
};

// beginFrame 的完整输入。
struct FrameInfo
{
    const scene::World *world = nullptr;
    const View *view = nullptr;
    double timeSeconds = 0.0;
};

} // namespace my3d::render
