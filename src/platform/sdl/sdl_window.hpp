#pragma once

// my3d::platform —— SDL2 窗口：原生窗口句柄 + 键鼠采集，IWindow 的图形后端实现。
//
// 职责边界（docs/architecture.md §3）：本类只做「窗口 + 输入」——产出语义按键
// （render::Button）、边沿事件（render::InputEvent）与像素尺寸。它**不含**相机数学
// （yaw/pitch → front 的换算属于 render/camera_controller.hpp）、不含渲染循环、
// 不创建 SwapChain、不建方向光。旧的 src/window.hpp 把这四件事塞在同一个类里
// （297 行，SDL 初始化 + viewport/光照 + 相机 + 主循环），于是终端模式下整套逻辑
// 作废、手感还得重写一遍。这个类存在的意义就是不重复那个错误。
//
// 与 TtyWindow 是**姊妹实现**：同一个 IWindow 契约、同一套语义按键、同样只在真的
// 变化时才追加 Resize 事件、同样的「构造失败即抛 std::runtime_error、析构幂等收尾」
// 纪律。两者互不知道对方存在 —— 本文件不 include platform/tty/ 下任何东西，
// 反向亦然；app 层按配置二选一。手感一致靠的是同一份语义映射，不是共享代码。
//
// 几条刻意的设计选择，连同为什么：
//
//  * **平台层不 include 任何 Filament 头。** 只前向声明 `SDL_Window`，所有 SDL 头
//    都留在 .cpp。后端与平台正交是架构硬要求：Filament 的 createSwapChain() 只
//    需要一个 void*，那本类就只给一个 void*，不把 Filament 的类型（乃至它的枚举）
//    拖进窗口抽象。附带好处很实在：`<SDL2/SDL_syswm.h>` 会间接引入 X11 的 X.h，
//    那里的 `Success` 宏会撞 Filament 的同名枚举 —— 旧代码因此在**头文件**里
//    `#undef Success`，把「平台层包含后端层」写进了包含关系。不 include 就没有这
//    个问题，也就永远不需要 #undef。
//  * **鼠标位移用 SDL 的相对量（xrel/yrel），不用「绝对坐标相减」。** 抓取鼠标时
//    SDL 会把指针钉在窗口内，绝对坐标到边缘就饱和（不再变化），此时 `x - lastX_`
//    恒为 0、转向直接死掉；相对量才是相对模式下的正确来源。旧的 window.hpp 用
//    `x - lastX_` **并且**维护 lastX_/lastY_ 两个成员，本实现两者都不需要：xrel/yrel
//    本身就是增量，「上一帧」这个状态根本不存在。
//  * **pointerCaptured_ 默认关闭。** 构造即抓取会让用户第一次进窗口就发现鼠标
//    不见了（且在放开之前拿不回来），这是最容易被当成 bug 的行为。改成由调用方
//    显式 setPointerCaptured(true) 进入 FPS 手感 —— 抓取是策略，不该由窗口层替
//    使用者决定。
//  * **三个 Toggle 边沿事件（F / C / G）在这里必须接上。** TTY 后端漏了它们，于是
//    线框 / 剔除 / 边线的开关在终端里根本没有入口，RenderSettings 里的选项成了死
//    代码 —— 「按键存在但没人翻译」正是新架构要修的缺陷。这里按边沿触发（只在
//    SDL_KEYDOWN 且 !repeat 时产生），因为它们是开关而不是持续量：连发会让一帧
//    翻好几次，最终值取决于队列长度这种不可预测的东西。
//  * **一个进程一个 SDL 窗口。** SDL_Init / SDL_Quit 是**进程级**状态，同时在栈上
//    放两个 SdlWindow 会让第二个析构时把第一个的子系统一起关掉。这与 TtyWindow
//    「termios 是设备级状态」是同一类约束，因此同样不加锁、不假装支持多实例。
//
// 用法（与 platform/window.hpp 的契约一致）：
//
//     SdlWindow window;                       // 失败即抛 std::runtime_error
//     while (window.poll(buttons, events)) {  // buttons 是「完全覆盖」，见下
//         ...                                  // events 由调用方每帧先 clear()
//     }
//
// poll() 的语义（契约原文在 platform/window.hpp，这里最易写错）：
//   * buttons 完全覆盖：poll 开头就按「本帧结束时的按键状态」整体改写，不是累加。
//   * events **只追加**，本类绝不清空调用方的 vector —— 谁拥有容器谁负责 clear()。

#include "platform/window.hpp"

#include <cstdint>
#include <vector>

// 前向声明而不是 include <SDL2/SDL.h>：见上面「不 include 任何后端头」的说明。
// 这也让本头可以被任何 TU 引入而不污染它（SDL 的宏定义相当霸道）。
struct SDL_Window;

namespace my3d::platform
{

class SdlWindow : public IWindow
{
public:
    // SDL 视频子系统或窗口创建失败时抛 std::runtime_error，消息里带上 SDL_GetError()。
    // 抛出前一定已经回滚掉本次构造自己申请到的资源（析构不会为半成品对象运行），
    // 所以调用方可以安全地捕获后改用别的后端。
    SdlWindow();
    ~SdlWindow() override;

    SdlWindow(const SdlWindow &) = delete;
    SdlWindow &operator=(const SdlWindow &) = delete;
    SdlWindow(SdlWindow &&) = delete;
    SdlWindow &operator=(SdlWindow &&) = delete;

    // 排空 SDL 事件队列 → 语义按键（完全覆盖 buttons）+ 本帧边沿事件（追加到 events）
    // + 尺寸缓存更新。返回 false 表示该结束主循环：ESC / SDL_QUIT / 窗口关闭。
    bool poll(render::ButtonState &buttons, std::vector<render::InputEvent> &events) override;

    // **像素**宽高（SDL_GetWindowSizeInPixels 的口径），区别于 TtyWindow 的字符行列。
    // Filament 的 viewport 与 swapchain 都要像素，所以这里不返回窗口坐标尺寸。
    uint32_t width() const noexcept override { return width_; }
    uint32_t height() const noexcept override { return height_; }

    const char *name() const noexcept override;

    // ---------------------------------------------------------------------
    // 以下能力是 SDL 平台特有的，**不属于** IWindow：终端没有 swapchain，也就没有
    // 原生句柄；TTY 也没有相对鼠标。把它们挡在 IWindow 之外，是为了不让「窗口的
    // 通用契约」长成两个后端的并集。
    // ---------------------------------------------------------------------

    // 交给 Filament createSwapChain() 的原生窗口（X11 Window / Wayland wl_surface）。
    // 每次调用都重新查询，不缓存：SDL_GetWindowWMInfo 只是填充一个结构体，而缓存
    // 反过来要处理「SDL 在 resize / 全屏切换时重建 surface」的失效问题。
    // 取不到（SDL 报错，或运行时的视频驱动与编译期选定的宏不一致）返回 nullptr ——
    // 调用方应把它当作「无法创建 swapchain」而不是当有效句柄用。
    void *nativeWindowHandle() const noexcept;

    // 本帧累积的鼠标位移（像素）。poll() 开头清零，poll() 之后读到的是这一帧的增量，
    // 连读两次不会叠加。方向：x 向右为正、y 向下为正（沿用 SDL 的屏幕坐标约定，
    // 「向上看」的符号由 render/camera_controller.hpp 决定）。
    float pointerDeltaX() const noexcept { return pointerDeltaX_; }
    float pointerDeltaY() const noexcept { return pointerDeltaY_; }

    // 本帧累积的滚轮量（一格 = 1.0，向上为正；高精度滚轮/触摸板按 SDL 的 preciseY
    // 连续累积）。同样按帧清零。
    float wheelDelta() const noexcept { return wheelDelta_; }

    // 鼠标是否被抓取（相对模式）。默认 false，理由见文件头。
    bool pointerCaptured() const noexcept { return pointerCaptured_; }

    // 开关相对鼠标模式（SDL_SetRelativeMouseMode）：开启后指针隐藏并被钉住，
    // SDL_MOUSEMOTION 的 xrel/yrel 给出连续位移，这是 FPS 视角需要的行为。
    //
    // noexcept 且失败静默：SDL 拒绝（视频驱动不支持相对模式）时保持原状态不变 ——
    // 「以为抓取了其实没抓」比「抓取失败」难查得多。已知残余现象：切换抓取的瞬间
    // SDL 可能因指针 warp 补发一次跳变位移，本类不吞它（吞掉就要引入「有一帧要
    // 丢弃」的隐式状态）；调用方可以在切换后的那一帧忽略 pointerDelta*。
    void setPointerCaptured(bool on) noexcept;

private:
    // 1280x720：旧 window.hpp 的窗口尺寸，保持不变，免得手感/截图基线一起变。
    static constexpr int kDefaultWidth = 1280;
    static constexpr int kDefaultHeight = 720;

    // 幂等收尾，按申请的逆序：SDL_DestroyWindow → SDL_Quit。析构与构造失败路径共用
    // （这是 TtyWindow::restore() 的同款纪律，理由也一样：半成品对象不会走析构）。
    // noexcept：析构路径上不能抛，且此时失败也无法补救。
    void shutdown() noexcept;

    // 按编译期选定的 SDL 视频驱动从 wmInfo 里取原生句柄，见 .cpp 里的 #if。
    void *resolveNativeHandle() const noexcept;

    // 查询像素尺寸。失败（窗口还没实现 / 已销毁 / SDL 返回 0）返回 false，调用方
    // 保留上一次的有效值 —— 0 宽高会让下游算 aspect 时除零。
    bool queryPixelSize(uint32_t &outWidth, uint32_t &outHeight) const noexcept;

    // 尺寸变化的统一入口：过滤掉 0 尺寸（最小化）与「和缓存一样」的重复值，只有真
    // 变了才追加 Resize 边沿事件 —— 重复的 Resize 会让下游反复重建网格。
    void applyResize(int32_t eventWidth, int32_t eventHeight,
                     std::vector<render::InputEvent> &events);

    SDL_Window *window_ = nullptr;
    bool videoInited_ = false;   // SDL_Init 是否成功过（决定析构要不要 SDL_Quit）
    bool running_ = true;        // SDL_QUIT / 窗口关闭后置 false

    uint32_t width_ = static_cast<uint32_t>(kDefaultWidth);
    uint32_t height_ = static_cast<uint32_t>(kDefaultHeight);

    // 当前**实际按住**的键。必须自己记：SDL 只在按下/抬起时各给一个事件，而 buttons
    // 是「本帧状态」的完全覆盖 —— 若只把 KEYDOWN 那帧置位，长按就会变成只动一帧。
    // 这正是 TTY 那边用 stickyWindow 近似解决的问题（那边没有 key-up），SDL 有真
    // 的 KEYUP，所以这里可以用精确状态而不是时间近似。
    render::ButtonState held_{};

    bool pointerCaptured_ = false;

    // 本帧增量，poll() 开头清零、事件里累积（契约：读到的必须是「本帧」的值）。
    float pointerDeltaX_ = 0.0f;
    float pointerDeltaY_ = 0.0f;
    float wheelDelta_ = 0.0f;
};

} // namespace my3d::platform
