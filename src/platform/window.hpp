#pragma once

// my3d::platform —— 窗口与输入抽象。
//
// 关键不变量（见 docs/architecture.md §3）：平台层**只产出语义输入事件**，
// 不含任何相机数学、不含场景构建。旧的 window.hpp 把 SDL 初始化、SwapChain、
// 方向光创建、相机 yaw/pitch 计算、渲染循环全塞进一个 297 行的类里 —— 于是
// 终端模式下这套逻辑几乎全部作废，手感还得重写一遍。这里把它守住。
//
// 本层与 backend/ 正交：ASCII 后端配 TtyWindow，Filament 后端配 SdlWindow。
// 两者之间没有依赖关系，可以自由组合。

#include "render/input.hpp"

#include <cstdint>
#include <vector>

namespace my3d::platform
{

class IWindow
{
public:
    virtual ~IWindow() = default;

    // 收集本帧输入。返回 false 表示窗口已失效（终端关闭、SDL_QUIT 或 ESC），
    // 调用方应结束主循环。
    //
    // 契约：buttons 会被**完全覆盖**（不是累加）；events 只**追加**本帧新产生的
    // 边沿事件，调用方负责在每次 poll 前 clear()。把「追加 vs 覆盖」写死在这里，
    // 是因为两种语义混用是输入层最典型的 bug 来源。
    virtual bool poll(render::ButtonState &buttons,
                      std::vector<render::InputEvent> &events) = 0;

    // 可绘制区域的尺寸。TTY 后端是字符列数 / 行数，SDL 后端是像素宽高 ——
    // 调用方（ASCII 后端）只把它当作网格尺寸，不关心单位。
    virtual uint32_t width() const noexcept = 0;
    virtual uint32_t height() const noexcept = 0;

    virtual const char *name() const noexcept = 0;
};

} // namespace my3d::platform
