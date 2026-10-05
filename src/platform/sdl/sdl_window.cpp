#include "platform/sdl/sdl_window.hpp"

// SDL 的头只出现在这个 .cpp 里：头文件里连 SDL_Window 都是前向声明（正交性的理由
// 见 sdl_window.hpp 的文件头）。附带好处显在这里 —— `<SDL2/SDL_syswm.h>` 会间接
// include X11 的 X.h，那里的 `Success` 宏会污染它之后被 include 的一切；本 TU 里
// 它后面只剩标准库头，而且**不含任何 Filament 头**（架构硬要求：平台层与后端层
// 正交），所以不需要旧代码那句 `#undef Success`。
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <stdexcept>
#include <string>

// 原生句柄的取法必须在编译期选定：wmInfo.info 是联合体，只有对应
// SDL_VIDEO_DRIVER_* 被定义时才有那个成员（SDL_config.h 按 SDL 的构建配置定义它们；
// 本仓库的 CMake 目标 my3d_platform_sdl 另加 WAYLAND）。
// 这里**故意没有**编译期守卫（旧代码里的 `#if !defined(X11) && !defined(WAYLAND)
// #error`）。它在本平台永不触发：SDL_config_unix.h（SDL 2.32.72）**无条件**同时定义
// X11 与 WAYLAND 两个宏，与「本构建想支持哪一个」无关 —— 一个恒为假的判断只是安全
// 剧场，留着比删掉更坏，因为它让人以为「已经检查过了」。真正有效的检查有两处，都不
// 需要这个守卫：构建侧由 CMake 的 target_compile_definitions 显式声明
// SDL_VIDEO_DRIVER_WAYLAND（src/CMakeLists.txt，那才是构建意图的所在）；运行侧由
// 下面的 resolveNativeHandle() 按**运行时**的 wmInfo.subsystem 分支，拿不到对应字段
// 就返回 nullptr —— 这是头文件写明的契约（「驱动与编译期宏不一致时当作无法创建
// swapchain」），比在构造函数里抛异常更合适：驱动不匹配时窗口本身仍然可用。

namespace my3d::platform
{
namespace
{

// 物理键位 → 语义按键。键盘映射与 platform/tty/tty_input.cpp 保持一致；
// 窗口另外提供鼠标视角与滚轮缩放。
bool mapScancode(SDL_Scancode scancode, render::Button &out) noexcept
{
    switch (scancode)
    {
    case SDL_SCANCODE_W:
        out = render::Button::MoveForward;
        return true;
    case SDL_SCANCODE_S:
        out = render::Button::MoveBackward;
        return true;
    case SDL_SCANCODE_A:
        out = render::Button::MoveLeft;
        return true;
    case SDL_SCANCODE_D:
        out = render::Button::MoveRight;
        return true;
    case SDL_SCANCODE_E:
        out = render::Button::MoveUp;
        return true;
    case SDL_SCANCODE_Q:
        out = render::Button::MoveDown;
        return true;
    case SDL_SCANCODE_H:
        out = render::Button::LookLeft;
        return true;
    case SDL_SCANCODE_L:
        out = render::Button::LookRight;
        return true;
    case SDL_SCANCODE_J:
        out = render::Button::LookDown;
        return true;
    case SDL_SCANCODE_K:
        out = render::Button::LookUp;
        return true;
    // ESC 当作**持续按键**（Button::Quit）而不是另外的布尔标志：这样「怎么算退出」
    // 的判定与 TtyWindow 完全同构（都是看 buttons 里的 Quit），app 层不必按后端分支。
    case SDL_SCANCODE_ESCAPE:
        out = render::Button::Quit;
        return true;
    default:
        // 未映射的键不是错误：键盘上的大多数键与本应用无关，静默忽略是正确行为。
        return false;
    }
}

// 两个平台共用 F（线框）和 C（剔除）。
//
// width/height 对所有非 Resize 事件都写 0（而不是留空）：InputEvent 是聚合体，
// 显式写 0 让「这两个字段只在 Resize 时有效」这件事在代码里可见。
void pushEdgeEvent(SDL_Scancode scancode, std::vector<render::InputEvent> &events)
{
    using Type = render::InputEvent::Type;
    switch (scancode)
    {
    case SDL_SCANCODE_F:
        events.push_back(render::InputEvent{Type::ToggleWireframe, 0, 0});
        break;
    case SDL_SCANCODE_C:
        events.push_back(render::InputEvent{Type::ToggleCulling, 0, 0});
        break;
    default:
        break;
    }
}

} // namespace

SdlWindow::SdlWindow()
{
    // 只初始化 VIDEO | EVENTS。SDL_INIT_EVERYTHING 会顺手拉起音频、手柄、定时器 ——
    // 一个窗口类去抢音频设备属于越权初始化，而且会让「谁该 SDL_Quit」更难说清。
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        // 先把错误文本抄下来：下面的 SDL_Quit() 会重置 SDL 的错误状态，之后再调
        // SDL_GetError() 只会拿到空串，而这句话是异常里唯一的线索。
        const std::string reason = SDL_GetError();
        // 与下面 SDL_CreateWindow 失败的路径对称地收尾：SDL_Init 失败时 SDL 可能已经
        // 部分初始化了子系统（VIDEO 成功而 EVENTS 失败），SDL 自己不会回滚，所以这里
        // 必须补上 SDL_Quit()（SDL 官方也要求它「即使 SDL_Init 失败也要调用」）。
        // videoInited_ 此刻**仍是 false**（它只在初始化成功后置位），显式写出来是为了
        // 让两条失败路径留下的对象状态完全一致 —— 半成品对象不会走析构（构造抛异常时
        // 析构函数不运行），状态只能在这里定死。
        SDL_Quit();
        videoInited_ = false;
        throw std::runtime_error("SdlWindow: SDL_Init 失败: " + reason);
    }
    videoInited_ = true;

    // SDL_WINDOW_VULKAN：Filament 走 Vulkan 后端，窗口必须能被 Vulkan 直接呈现。
    // SDL_WINDOW_RESIZABLE 让下面的 Resize 路径真的会被走到（不可调整大小的窗口上
    // 那段代码是死代码）。SDL_WINDOW_SHOWN 显式写出，不依赖 SDL 的隐式默认。
    window_ = SDL_CreateWindow("my3d", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               kDefaultWidth, kDefaultHeight,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (window_ == nullptr)
    {
        // 先把错误文本抄下来：shutdown() 里的 SDL_Quit 会重置 SDL 的错误状态，之后
        // 再调 SDL_GetError() 只会拿到空串，而这句话是异常里唯一的线索。
        const std::string reason = SDL_GetError();
        // 半成品对象不会走析构（构造抛异常时析构函数不运行）：SDL_Init 已经成功了，
        // 必须在这里自己回滚。这正是 TtyWindow 注释里强调的同一条纪律。
        shutdown();
        throw std::runtime_error("SdlWindow: SDL_CreateWindow 失败: " + reason);
    }

    // 初始尺寸不能指望 WINDOWEVENT（创建时还没有任何事件），主动查一次。查询失败就
    // 保留 kDefaultWidth/kDefaultHeight —— 它们是刚刚传给 SDL_CreateWindow 的值，
    // 因此是「与真实窗口一致」的兜底，而不是随便填的魔数。
    (void)queryPixelSize(width_, height_);
}

SdlWindow::~SdlWindow()
{
    shutdown();
}

void SdlWindow::shutdown() noexcept
{
    // 幂等：每个分支先看自己的状态再清掉，重复调用（析构 + 构造失败路径可能连着走）
    // 都是空操作。顺序与申请的逆序：先窗口，后子系统。
    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (videoInited_)
    {
        // SDL_Quit 是**进程级**收尾：它会关掉所有 SDL 子系统，不只是本对象申请过的
        // 那部分。这正是「一个进程一个 SdlWindow」这条约束的由来（见文件头），也是
        // 这里不做「引用计数」的原因 —— 假装支持多实例只会把冲突推迟到更难查的地方。
        SDL_Quit();
        videoInited_ = false;
    }
}

void *SdlWindow::nativeWindowHandle() const noexcept
{
    // 公共入口只是转调：真正的判定与 #if 分支在 resolveNativeHandle() 里（放在 .cpp
    // 的匿名/私有区域，是为了让 SDL_syswm.h 与那堆编译期分支完全不出现在头文件中）。
    return resolveNativeHandle();
}

void *SdlWindow::resolveNativeHandle() const noexcept
{
    if (window_ == nullptr)
        return nullptr;

    // 先清零再填 version：SDL 用 version 字段做 ABI 校验，而查询失败时它可能完全不
    // 碰结构体 —— 那时读 subsystem 就是读未初始化的值。
    SDL_SysWMinfo wmInfo{};
    SDL_VERSION(&wmInfo.version);
    // 返回值不单独处理：失败时 subsystem 保持 SDL_SYSWM_UNKNOWN，下面的 switch 自然
    // 落到 default 返回 nullptr —— 这就是「取不到」的一致出口。
    (void)SDL_GetWindowWMInfo(window_, &wmInfo);

    // 按**运行时**的 subsystem 选字段，而不是一头扎进某个编译期宏分支。两者回答的是
    // 不同的问题：编译期宏说的是「这个构建支持哪些后端」（CMake 的 my3d_platform_sdl
    // 定义 WAYLAND），而实际开出哪个窗口由 SDL 在运行时决定（会话类型、SDL_VIDEODRIVER、
    // 驱动可用性都会影响它）。用 Wayland 的字段去解释一个 X11 窗口会得到一个看着有效
    // 实则无效的指针，那种错误要到 Filament 创建 swapchain 时才炸，且报错毫无指向性。
    //
    // Wayland 分支写在 X11 前面，对应「Wayland 会话优先」：在 XWayland 下
    // wmInfo.info.x11.window 指向的是 X11 的窗口，而当前构建拿它去交给 Vulkan 是不
    // 成立的。真正的判据仍是 subsystem，顺序只是让两个宏都定义时的默认读法更安全。
    switch (wmInfo.subsystem)
    {
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        return wmInfo.info.wl.surface;   // struct wl_surface*
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        // X11 的 Window 是 XID（unsigned long 的位模式），按位重解释成 void*：
        // Filament 只把它当作不透明的原生句柄，不会解引用它。
        return reinterpret_cast<void *>(wmInfo.info.x11.window);
#endif
    default:
        return nullptr;
    }
}

bool SdlWindow::queryPixelSize(uint32_t &outWidth, uint32_t &outHeight) const noexcept
{
    if (window_ == nullptr)
        return false;

    int w = 0;
    int h = 0;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    // 契约（IWindow::width）要的是**像素**。SDL_GetWindowSize 给的是窗口坐标尺寸，
    // 在 HiDPI / 分数缩放下两者不同，而 Filament 的 viewport 只认像素。本构建没有
    // 请求 SDL_WINDOW_ALLOW_HIGHDPI，所以两者目前数值相同 —— 这里仍然用像素 API，
    // 是为了将来加上那个 flag 时不会静默错位。
    SDL_GetWindowSizeInPixels(window_, &w, &h);
#else
    // 2.26 之前没有像素级 API，只能退到窗口坐标尺寸。写明这一点，免得读代码的人
    // 以为这里的数值依定义就是像素。
    SDL_GetWindowSize(window_, &w, &h);
#endif

    // 最小化、或刚创建还没被 WM 实现时可能是 0。当作「查询失败」，让调用方保留上一次
    // 的有效值 —— 0 行列会让下游算 aspect 时除零，这在 TtyWindow 那边是同样的处理。
    if (w <= 0 || h <= 0)
        return false;

    outWidth = static_cast<uint32_t>(w);
    outHeight = static_cast<uint32_t>(h);
    return true;
}

void SdlWindow::applyResize(int32_t eventWidth, int32_t eventHeight,
                            std::vector<render::InputEvent> &events)
{
    // 最小化时 SDL 报 0 x 0。0 不是「很小的窗口」而是「没有可绘制区域」：放进缓存会
    // 让下游拿 0 去算 aspect / 建 swapchain。丢掉它并保留上次的有效尺寸。
    if (eventWidth <= 0 || eventHeight <= 0)
        return;

    // 事件的 data1/data2 是**窗口坐标**尺寸，而 width_/height_（以及 IWindow 的契约，
    // 见 platform/window.hpp）是**像素**。两者只在「没请求 SDL_WINDOW_ALLOW_HIGHDPI
    // 且缩放为 1」时才数值相等，依赖这个巧合会让将来打开那个 flag 时 Resize 事件用
    // 窗口坐标覆盖掉像素尺寸，而下一次 queryPixelSize() 又把它改回像素 —— 缓存值在
    // 两个口径之间来回跳。所以以像素查询为准（与 queryPixelSize 同源）；查不到（窗口
    // 还没被 WM 实现、或 SDL 返回 0）才回退到事件给的窗口坐标，让「真的变了才发事件」
    // 这条边沿语义在查询不可用时仍然可用。
    // 追加的 Resize 事件也带像素，与缓存同口径：下游拿它去 resize swapchain
    // （samples/viewer_asset.cpp），单位必须与 IWindow::width() 一致。
    uint32_t w = 0;
    uint32_t h = 0;
    if (!queryPixelSize(w, h))
    {
        w = static_cast<uint32_t>(eventWidth);
        h = static_cast<uint32_t>(eventHeight);
    }

    // 只有真的变了才追加：events 是**边沿**事件，重复的 Resize 会让下游反复重建网格
    // 与 swapchain。反过来，拖动窗口时 SDL 会为每个中间尺寸各发一个事件，这里**不做
    // 去抖**（只合并同值）：中间尺寸都是有效的可绘制尺寸，跳过它们会让画面在大尺寸下
    // 渲染进小 buffer。
    if (w == width_ && h == height_)
        return;

    width_ = w;
    height_ = h;
    events.push_back(render::InputEvent{render::InputEvent::Type::Resize, w, h});
}

bool SdlWindow::poll(render::ButtonState &buttons, std::vector<render::InputEvent> &events)
{
    // 本帧增量的起点：清零放在**所有**提前返回之前，因此「poll 之后读到的就是本帧的
    // 增量」。放在这里而不是事件循环之后，是为了让指针/滚轮的语义与 buttons 一致 ——
    // 都是「poll 返回后代表本帧」。
    // 清零点**不能**挪到下面 window_ == nullptr 那条提前返回之后：那条路径会跳过清零，
    // 于是对已失效的窗口再 poll 一次读到的是上一帧的位移 —— 同一份增量被消费两次，
    // 违反「poll 之后读到的就是本帧增量」这条契约。
    pointerDeltaX_ = 0.0f;
    pointerDeltaY_ = 0.0f;
    wheelDelta_ = 0.0f;

    // 窗口不可用（已经 shutdown）或压根没起来：结束主循环，而不是去排空一个不存在的
    // 窗口的事件队列 —— 后者会把 SDL 的全局错误状态搅乱，掩盖真正的问题。
    if (window_ == nullptr)
    {
        buttons.clear();
        held_.clear();
        return false;
    }

    SDL_Event event;
    // 一帧内排空整个队列（SDL_PollEvent 返回 0 即队列空，不阻塞）。把剩余事件留给
    // 下一帧会让输入比画面晚一帧，长按时的表现是「按键有延迟」。
    while (SDL_PollEvent(&event) != 0)
    {
        switch (event.type)
        {
        case SDL_QUIT:
            // 会话结束 / WM 要求所有窗口退出。这是「主循环该结束」，不是错误。
            running_ = false;
            break;

        case SDL_WINDOWEVENT:
            switch (event.window.event)
            {
            case SDL_WINDOWEVENT_CLOSE:
                // 点了标题栏的关闭按钮：WM 在请求关闭这个窗口。
                running_ = false;
                break;
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                // 两个都要接：RESIZED 是用户/系统改的尺寸，SIZE_CHANGED 还会覆盖
                // 「API 调用改尺寸」这条路径。只接其中一个就会漏掉另一条。
                applyResize(event.window.data1, event.window.data2, events);
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                // 失焦时 WM 不会再补发 KEYUP（键盘焦点已经不在本窗口）。不清掉按下
                // 状态的话，Alt-Tab 切走时按着的 W 会在切回来之后继续「按住」，表现为
                // 相机自己往前跑 —— 而用户无法通过松手来停止它。
                held_.clear();
                break;
            default:
                break;
            }
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            const bool isDown = (event.type == SDL_KEYDOWN);

            render::Button button{};
            if (mapScancode(event.key.keysym.scancode, button))
                held_.set(button, isDown);

            // 边沿事件只在「真的按下」时产生一次。event.key.repeat != 0 是 SDL 的
            // 自动连发（按住不放），开关动作必须忽略：连发会让同一个 Toggle 在一帧里
            // 翻好几次，最终状态取决于事件队列长度，也就是不可预测。
            if (isDown && event.key.repeat == 0)
                pushEdgeEvent(event.key.keysym.scancode, events);
            break;
        }

        case SDL_MOUSEMOTION:
            // 累积相对量（xrel/yrel），不做绝对坐标相减 —— 理由见文件头：抓取鼠标时
            // 绝对坐标会饱和到边缘。非抓取模式下 SDL 同样给出 xrel/yrel，所以这里
            // 不需要「抓取中/未抓取」的分支，两种模式共用一条路径。
            pointerDeltaX_ += static_cast<float>(event.motion.xrel);
            pointerDeltaY_ += static_cast<float>(event.motion.yrel);
            break;

        case SDL_MOUSEWHEEL:
            // 只累积，不当场改 fov：fov 是相机状态（render/camera_controller.hpp）。
            // 旧代码在这里直接改 cameraCPU_.fovY —— 那正是「平台层越界改了相机」的
            // 典型，也是终端模式下这段逻辑全废的原因。
            //
            // 数值取整数 y（普通滚轮一格 = ±1），但 high-resolution 滚轮 / 触摸板在
            // SDL 里走的是 preciseY（2.0.18 起），而 y 是它的取整 —— 0.3 格这类小
            // 增量取整后就是 0，只读 y 会把整段平滑滚动吞掉。所以 y 为 0 而 preciseY
            // 非 0 时用 preciseY 兜底：两者是「取一」而不是相加，不会重复计数。
            // 结果是 wheelDelta 的量纲统一为「格」，普通滚轮与高精度输入都能用。
            wheelDelta_ += (event.wheel.y != 0) ? static_cast<float>(event.wheel.y)
                                                : event.wheel.preciseY;
            break;

        default:
            // 其余事件（文字输入、鼠标键、手柄…）本层不消费。尤其是按键的日志：
            // 每帧打印每个按键会把 stdout 变成瓶颈，平台层不该做这件事。
            break;
        }
    }

    // buttons 是**完全覆盖**语义（契约原文在 platform/window.hpp）：把本帧结束时的
    // 真实按下状态整体拷过去。用 held_ 而不是「只在 KEYDOWN 那帧置位」，是因为
    // buttons 描述的是「本帧按键是否按下」这个**持续状态**，而 SDL 只在变化时给事件。
    buttons = held_;

    if (!running_)
        return false;
    // ESC 与 TTY 走同一条判定路径（都看 Button::Quit），app 层因此不必按后端分支。
    return !buttons.isDown(render::Button::Quit);
}

const char *SdlWindow::name() const noexcept
{
    return "sdl";
}

void SdlWindow::setPointerCaptured(bool on) noexcept
{
    if (window_ == nullptr)
        return;

    // 调用失败（某些驱动/合成器不支持相对模式）时**保持原值不变**：声称「已抓取」
    // 却没抓，会让调用方按相对量的口径去消费位移，而事件里给的其实是另一套量 ——
    // 那种错位极难定位，不如让状态如实反映现实。
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) != 0)
        return;

    pointerCaptured_ = on;
}

} // namespace my3d::platform
