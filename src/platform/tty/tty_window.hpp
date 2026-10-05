#pragma once

// my3d::platform —— TTY 窗口（无 SDL）：termios raw mode + TIOCGWINSZ + SIGWINCH。
//
// 职责边界（docs/architecture.md §3）：只做终端状态与输入采集，产出语义按键与
// 尺寸事件；不含相机数学、不含渲染、不 include backend/ 下的任何东西。ASCII 后端
// 配 TtyWindow、Filament 后端配 SdlWindow，两者互不知道对方存在。
//
// 不变量与陷阱（照 §6.5 / §10 定）：
//
//  * **终端状态必须恢复。** raw mode 未恢复会把用户的 shell 变成无回显、无行编辑
//    的裸终端。因此进入 raw mode 之后的**所有**失败路径（构造抛异常、析构）都要
//    走 restore()，且 restore() 自身幂等、可重复调用。它同时负责退出交替屏幕
//    缓冲（ESC[?1049l）与恢复光标可见（ESC[?25h）。
//  * **一个进程一个终端窗口。** termios 是设备级状态、信号处置是进程级状态，
//    同时在栈上放两个 TtyWindow 会互相踩（第二个保存的是第一个改过的状态）。
//    本类不为此加锁，因为那只会掩盖「谁在读终端」的混乱。
//  * **stdin 与 stdout 都必须是终端。** 读走 stdin，转义序列写 stdout；stdout 被
//    重定向时进交替屏幕会把控制码写进文件，所以这种配置直接构造失败，而不是
//    「尽力而为」。
//  * SIGKILL / 掉电无法恢复终端，这是任何 raw mode 程序都有的残余风险。
//  * 尺寸以 ioctl(TIOCGWINSZ) 为**准**，SIGWINCH 只当提示：信号可能丢失、被别处
//    处置或与帧错位，所以每次 poll() 都直接复查，只有数值真的变了才产生 Resize
//    边沿事件。
//
// 用法（与 platform/window.hpp 的契约一致）：
//
//     TtyWindow window;                       // 失败即抛 std::runtime_error
//     while (window.poll(buttons, events)) {  // events 由调用方每帧先 clear()
//         ...                                  // buttons 是「完全覆盖」语义
//     }

#include "platform/tty/tty_input.hpp"
#include "platform/window.hpp"

#include <signal.h>
#include <termios.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace my3d::platform
{

class TtyWindow : public IWindow
{
public:
    // 终端不可用（stdin/stdout 不是 tty、tcgetattr/tcsetattr 失败）时抛
    // std::runtime_error；抛出前一定已经恢复终端状态，调用方可以安全地捕获后
    // 退回别的后端。
    TtyWindow();
    ~TtyWindow() override;

    TtyWindow(const TtyWindow &) = delete;
    TtyWindow &operator=(const TtyWindow &) = delete;
    TtyWindow(TtyWindow &&) = delete;
    TtyWindow &operator=(TtyWindow &&) = delete;

    // 非阻塞排空 stdin → 语义按键（完全覆盖 buttons）+ 尺寸变化（追加到 events）。
    // 返回 false 表示该结束主循环：收到 Quit，或 stdin 真的 EOF（终端窗口被关）。
    bool poll(render::ButtonState &buttons, std::vector<render::InputEvent> &events) override;

    // 字符列数 / 行数。ASCII 后端把它当网格尺寸，不关心单位。
    uint32_t width() const noexcept override { return width_; }
    uint32_t height() const noexcept override { return height_; }

    const char *name() const noexcept override;

    // 手感微调（键位粘滞窗口 / ESC 收尾超时），语义见 tty_input.hpp。
    TtyInput &input() noexcept { return input_; }

private:
    // 进入 raw mode + 交替屏幕 + 隐藏光标 + 装 SIGWINCH。任一关键步骤失败返回 false，
    // 由调用方负责 restore()（构造中途失败必须自己收尾，析构不会为半成品对象运行）。
    bool enterTerminal();
    // 幂等恢复，按进入的逆序。noexcept：析构路径上不能抛，且此时写失败也无法补救。
    void restore() noexcept;
    // 排空 stdin。eof 置 true 表示终端真的没了，而不是「本帧暂无数据」。
    void drainInput(std::chrono::steady_clock::time_point now, bool &eof);
    bool querySize(uint32_t &outWidth, uint32_t &outHeight) const noexcept;

    int fd_ = -1;        // 输入：STDIN_FILENO
    int outFd_ = -1;     // 输出：STDOUT_FILENO（转义序列）
    termios saved_{};    // 进入 raw mode 前的原始属性，恢复时写回
    struct sigaction oldWinch_{};

    bool rawActive_ = false;       // termios 是否已被本对象改成 raw
    bool screenActive_ = false;    // 是否已进入交替屏幕（含隐藏光标）
    bool winchInstalled_ = false;  // SIGWINCH 是否已被本对象接管

    // ioctl 失败时的兜底尺寸：0 行列会让下游算 aspect 时除零。
    uint32_t width_ = 80;
    uint32_t height_ = 24;

    TtyInput input_;
    render::ButtonState previousButtons_{};
};

} // namespace my3d::platform
