#pragma once

// my3d::backend::cpu —— 软件光栅化后端（像素输出）。
//
// 它是第一个「自研后端」，作用是证明 render::IRenderer / IRenderDevice 这层
// 抽象确实能在零第三方依赖下被实现。ASCII 后端是它的孪生兄弟：两者都继承
// CpuResourcePool、都调用 drawScene，只有「线性 RGB 出来之后怎么落地」不同。

#include "asset/image_data.hpp"
#include "backend/cpu/framebuffer.hpp"
#include "backend/cpu/resource_pool.hpp"
#include "backend/cpu/scene_draw.hpp"
#include "render/renderer.hpp"
#include "render/types.hpp"

#include <vector>

namespace my3d::backend::cpu
{

class CpuRenderer final : public render::IRenderer, public CpuResourcePool
{
public:
    // ---------------- IRenderer ----------------
    bool beginFrame(const render::FrameInfo &info) override;
    void endFrame() override;
    bool resize(uint32_t width, uint32_t height) override;
    const char *name() const noexcept override;
    bool readbackImage(asset::ImageData &out) override;

    // ---------------- 结果访问 ----------------
    //
    // present 不属于本类：把线性浮点缓冲交给窗口是**平台层**的事
    // （SDL / TTY / 离屏测试各自不同）。后端只负责把像素算出来。
    const Framebuffer &framebuffer() const noexcept { return framebuffer_; }

    const RasterStats &rasterStats() const noexcept { return rasterStats_; }
    const SceneDrawStats &sceneStats() const noexcept { return sceneStats_; }

    double lastFrameSeconds() const noexcept { return lastFrameSeconds_; }

private:
    Framebuffer framebuffer_;
    RasterStats rasterStats_;
    SceneDrawStats sceneStats_;
    std::vector<render::DrawItem> drawItems_;
    double lastFrameSeconds_ = 0.0;
};

} // namespace my3d::backend::cpu
