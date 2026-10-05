#pragma once

// my3d::platform —— TTY 输入：字节流 → 语义按键。
//
// 本模块**不知道窗口、termios、文件描述符的存在**：只吃「字节 + 当前时刻」，
// 吐出 render::Button。理由不是洁癖，而是可测性：真终端下验证 raw mode 恢复与
// 长按非常贵（且会干扰使用者），把解析抽干净就能在没有终端的进程里单测它；
// TtyWindow 于是只剩下 IO 与终端状态管理。
//
// 键位映射（本模块的对外文档；字符映射在 tty_input.cpp 中维护）：
//
//   w / s              → MoveForward / MoveBackward       前后
//   a / d              → MoveLeft    / MoveRight          左右**平移**（不是转身）
//   q / e              → MoveDown    / MoveUp             升降
//   h / l / j / k      → LookLeft / LookRight / LookDown / LookUp
//   f / c              → ToggleWireframe / ToggleCulling
//   单独 ESC           → Quit（判定方式见下面第 2 条）
//
// a/d 做平移而不做转身是有意的：转身由 hjkl 控制，若 a/d 也转身，终端里就再也
// 表达不出「平移」这个动作。语义动作到相机数学的换算见
// render/camera_controller.hpp —— 终端与 SDL 共用同一套手感。
//
// 下面三点是本模块最容易写错的地方，一次讲清：
//
// 1) **字节流会被 read(2) 任意切开。** 终端转义序列可能被内核拆成多段、分别到达；
//    任何「一次 read 拿一个完整序列」的写法都会随机误解析。所以这里的解析器是
//    一个**跨调用保持状态**的字符状态机（Ground / Escape / Csi）：未完成的前缀
//    留在对象里，下次 feed() 接着往里喂字节。
//
// 2) **ESC 天生有二义性**：它既是独立按键，又是一切转义序列的开头，只看字节
//    无法区分。这里用时间收尾：尾部悬空的 ESC 保持待定，调用方每帧调 tick(now)，
//    若该前缀静默超过 escapeTimeout（默认 50ms）就按「单独 ESC」结算成 Quit。
//    代价（明确接受）：ESC 之后在超时窗口内紧跟其它字节时，那串字节被当作序列
//    内容（Alt+键 一类）整段丢弃，而不是「Quit + 那个键」。这与 readline 的做法
//    一致，也是终端里唯一可实现的判定。
//
// 3) **TTY 没有 key-up 事件**（终端只发按下，不发抬起）。为了让「长按 = 持续
//    动作」成立，收到字符就把该键置 down，并让它保持 stickyWindow（默认 120ms）：
//    终端的 auto-repeat 频率（常见 25–30 次/秒）与帧率（ASCII 后端 15–30fps）
//    不同步，没有粘滞窗口时「按下」会在帧边界上抖动（表现为移动一顿一顿）。
//    粘滞量用 std::chrono::steady_clock 计量，与墙上时间跳变无关。
//    这只是近似：轻点一下也会产生约 120ms 的「按下」，于是那一下会带来一次
//    小幅位移 / 转角（约 0.12s × 速度）。这是刻意取舍 —— 移动与视角本来就是
//    连续按压操作，而「点一下完全不动」在无 key-up 的模型下无法表达。
//
// 线程约束：本类不是线程安全的，且刻意不加锁 —— 输入只在主循环线程里被触碰，
// 加锁反而会掩盖「谁在读终端」的混乱。

#include "render/input.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace my3d::platform
{

class TtyInput
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 120ms：足以跨过 ASCII 后端一帧（约 33–66ms）而不至于让松手后的动作拖沓。
    static constexpr std::chrono::milliseconds kDefaultSticky{120};
    // 50ms：要比「同一次按键里 ESC 与后续字节的间隔」（微秒级）大得多，又要比
    // 「人手两次独立按键的间隔」（通常 > 100ms）小得多。
    static constexpr std::chrono::milliseconds kDefaultEscapeTimeout{50};

    // 喂入原始字节，返回本批新解析出的按键（按解析顺序）。不完整的尾巴留在内部
    // 状态里，不产生任何按键。
    //
    // now 由调用方给，而不是这里取 steady_clock::now()：解析器因此是「字节 + 时间」
    // 的纯函数，可被确定性测试，也让 TtyWindow 一帧内只取一次时钟，避免同一帧里的
    // 按键用上不同的时间基准。
    std::vector<render::Button> feed(const char *bytes, size_t count, TimePoint now);

    // 时间驱动收尾。必须在每帧调用（即使本帧一个字节都没读到），否则「按一下 ESC
    // 然后不动」永远等不到 Quit，被截断的序列也会一直挂在状态里。
    // Escape 态超时 → Quit；Csi 态超时 → 整段丢弃（不产生按键）。
    std::vector<render::Button> tick(TimePoint now);

    // 当前按下集合。**完全覆盖** out（不累加），与 platform/window.hpp 里
    // IWindow::poll 的契约一致 —— 两种语义混用是输入层最典型的 bug 来源。
    void fillButtonState(TimePoint now, render::ButtonState &out) const noexcept;

    void setStickyWindow(std::chrono::milliseconds window) noexcept { sticky_ = window; }
    void setEscapeTimeout(std::chrono::milliseconds timeout) noexcept { escapeTimeout_ = timeout; }

    std::chrono::milliseconds stickyWindow() const noexcept { return sticky_; }
    std::chrono::milliseconds escapeTimeout() const noexcept { return escapeTimeout_; }

    // 有未完成的转义前缀（ESC / ESC[ 已到、后续字节还没来）。调试与测试用；
    // 正常路径不需要关心。
    bool hasPendingPrefix() const noexcept { return state_ != State::Ground; }

    // 丢弃未完成前缀与全部粘滞状态。终端关闭 / 重新进入时用，避免上一段会话的
    // 半个序列或「还按着」的键漏到新会话里。
    void reset() noexcept;

private:
    // Ground：正常字符。Escape：收到 ESC，还不知道它是独键还是序列开头。
    // Csi：ESC [ 或 ESC O 之后，正在收参数字节，直到 final byte。
    enum class State : uint8_t
    {
        Ground,
        Escape,
        Csi
    };

    // CSI 参数字节的上限。参数本身不参与按键映射；没有上界的话，一串垃圾数字能
    // 让状态机无限期地吞字节。
    static constexpr uint8_t kMaxParamBytes = 16;

    void consume(uint8_t byte, TimePoint now, std::vector<render::Button> &out);
    void emit(render::Button button, TimePoint now, std::vector<render::Button> &out);
    void resetPending() noexcept;

    State state_ = State::Ground;
    TimePoint lastByteTime_{};   // 前缀里最后一个字节的到达时刻，用于超时判定
    uint8_t paramCount_ = 0;

    // 每个键的「保持按下到什么时候」。用数组而非 map：键集封闭且很小，
    // 数组无分配、无分支。
    std::array<TimePoint, render::kButtonCount> releaseAt_{};

    std::chrono::milliseconds sticky_ = kDefaultSticky;
    std::chrono::milliseconds escapeTimeout_ = kDefaultEscapeTimeout;
};

} // namespace my3d::platform
