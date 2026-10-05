#include "backend/cpu/cpu_renderer.hpp"

#include <chrono>

namespace my3d::backend::cpu
{

bool CpuRenderer::beginFrame(const render::FrameInfo &info)
{
    const auto start = std::chrono::steady_clock::now();

    const bool ok = drawScene(info, *this, framebuffer_, rasterStats_, sceneStats_, drawItems_);

    const auto end = std::chrono::steady_clock::now();
    lastFrameSeconds_ = std::chrono::duration<double>(end - start).count();
    return ok;
}

void CpuRenderer::endFrame()
{
    // 软件后端的「提交」在 beginFrame 里已经同步完成了，这里没有排队的工作。
    // 保留这个空实现是刻意的：GPU 后端的 endFrame 会真的提交命令缓冲，
    // 调用方不该关心这个差异。
}

bool CpuRenderer::resize(uint32_t width, uint32_t height)
{
    return framebuffer_.resize(width, height);
}

const char *CpuRenderer::name() const noexcept { return "cpu"; }

bool CpuRenderer::readbackImage(asset::ImageData &out)
{
    if (framebuffer_.empty())
        return false;

    framebuffer_.toImage(out);
    return out.valid();
}

} // namespace my3d::backend::cpu
