#include "platform/tty/tty_window.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace my3d::platform
{
namespace
{

// SIGWINCH 处理函数里**只置标志**：不 IO、不分配、不调用非异步信号安全的函数。
// volatile sig_atomic_t 是标准保证「信号处理函数中的读写既不会被寄存器缓存、
// 也不会被撕裂」的唯一类型；用 bool 可能被优化掉，用 std::atomic 则会引入
// 非异步信号安全的实现细节。
volatile sig_atomic_t g_sigwinchPending = 0;

extern "C" void ttyHandleSigwinch(int)
{
    g_sigwinchPending = 1;
}

// 进入 / 退出终端模式的转义序列。写成常量而不是散落的字面量，是因为「恢复」必须
// 与「进入」严格配对 —— 少一条就破坏用户的 shell。
// kExit 里多了一条 SGR 复位（ESC[0m）：ASCII 后端每帧自己负责收尾颜色，但真出了
// 漏网的颜色码时，这几字节是「用户 shell 变彩色」与「一切正常」的区别。
constexpr char kEnterSequences[] = "\x1b[?1049h\x1b[?25l";   // 交替屏幕缓冲 + 隐藏光标
constexpr char kExitSequences[] = "\x1b[0m\x1b[?25h\x1b[?1049l";   // 复位属性 + 显示光标 + 退出交替屏幕

// write(2) 允许短写（终端上少见但必须处理）。返回 false 表示终端写不动了。
bool writeAll(int fd, const char *data, size_t size) noexcept
{
    size_t written = 0;
    while (written < size)
    {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n > 0)
        {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

} // namespace

TtyWindow::TtyWindow()
{
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
        throw std::runtime_error("TtyWindow: stdin 与 stdout 都必须是终端");
    fd_ = STDIN_FILENO;
    outFd_ = STDOUT_FILENO;

    if (!enterTerminal())
    {
        // 半成品对象不会走析构，失败路径必须自己收尾 —— enterTerminal() 可能已经
        // 改过 termios 或已经进了交替屏幕。
        restore();
        throw std::runtime_error("TtyWindow: 无法设置终端 raw mode");
    }

    if (!querySize(width_, height_))
    {
        // 少数终端（或刚好在 resize 中）返回 0 行列，保留头文件里的兜底尺寸。
        width_ = 80;
        height_ = 24;
    }
}

TtyWindow::~TtyWindow()
{
    restore();
}

bool TtyWindow::enterTerminal()
{
    if (::tcgetattr(fd_, &saved_) != 0)
        return false;

    termios raw = saved_;
    // 关 ICANON/ECHO 是 raw mode 的本体。关 ISIG 可避免 Ctrl-C 触发异步 SIGINT、
    // 绕过 RAII 恢复终端；两个平台统一用 ESC 退出。
    // 关 IXON 免得 Ctrl-S/Ctrl-Q 把输出流冻住（ASCII 后端要靠 stdout 出帧）。
    // 关 ICRNL 使回车保持 \r，行为可预测。
    // 保留 OPOST 是有意的：调用方写 '\n' 仍应正确换行，光标定位由 ASCII 后端
    // 自己发转义序列负责。
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | INLCR | BRKINT);

    // VMIN=0/VTIME=0 让 read(2) 非阻塞：没数据立刻返回 0。这同时是「必须先
    // poll(2) 判可读」的原因 —— 在这个模式下「read 返回 0」既可能是「暂无数据」
    // 也可能是「真 EOF」，只有 poll 的就绪信息能把两者分开（见 drainInput）。
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    // 进入用 TCSANOW：不丢弃用户启动前的预输入，那是他的按键而不是垃圾。
    if (::tcsetattr(fd_, TCSANOW, &raw) != 0)
        return false;
    rawActive_ = true;

    // 先标记「屏幕已被动过」再写：writeAll 可能只写出一半就失败（比如 pty 已经消失），
    // 那种情况下也必须补上退出序列，否则终端会停在交替屏幕里 —— 用户看到的是一个
    // 空屏，而不是自己原来的 shell。退出序列本身幂等，多写一次无害。
    screenActive_ = true;
    if (!writeAll(outFd_, kEnterSequences, sizeof(kEnterSequences) - 1))
        return false;

    struct sigaction action{};
    action.sa_handler = ttyHandleSigwinch;
    ::sigemptyset(&action.sa_mask);
    // SA_RESTART 的取舍：打开它，阻塞的 read(2) 会因 SIGWINCH 被自动重启而不是返回
    // EINTR。本类的输入是 VMIN=0 的非阻塞读 + 零超时 poll(2)，从不依赖 EINTR 作为
    // 唤醒信号，所以重启语义不会让这里漏掉 resize；反而关掉它会让调用方自己的阻塞
    // 调用莫名收到 EINTR。resize 每帧都用 ioctl 复查，也就不要求信号及时送达。
    action.sa_flags = SA_RESTART;
    // 装不上也不算致命：尺寸复查在 poll() 里无条件进行，信号只是额外的提示。
    if (::sigaction(SIGWINCH, &action, &oldWinch_) == 0)
    {
        winchInstalled_ = true;
        g_sigwinchPending = 0;
    }
    return true;
}

void TtyWindow::restore() noexcept
{
    // 幂等：每个分支先看自己的标志再清掉，重复调用是空操作。
    // 顺序与进入时相反：卸信号 → 显示光标 + 退出交替屏幕 → 恢复 termios。
    if (winchInstalled_)
    {
        ::sigaction(SIGWINCH, &oldWinch_, nullptr);   // 交还给原来的处置者
        winchInstalled_ = false;
        g_sigwinchPending = 0;
    }

    if (screenActive_)
    {
        // 写失败也无法补救（终端可能已经消失），析构路径上更不能抛。
        writeAll(outFd_, kExitSequences, sizeof(kExitSequences) - 1);
        screenActive_ = false;
    }

    if (rawActive_)
    {
        // TCSAFLUSH：丢掉 raw 期间残留的半个转义序列，别把它漏给 shell 当成输入。
        ::tcsetattr(fd_, TCSAFLUSH, &saved_);
        rawActive_ = false;
    }
}

bool TtyWindow::poll(render::ButtonState &buttons, std::vector<render::InputEvent> &events)
{
    if (!rawActive_)
    {
        // 已经恢复过终端（或压根没起来）：再轮询只会把 shell 的字节当成游戏输入。
        buttons.clear();
        return false;
    }

    // 一帧只取一次时钟：同帧内所有按键、粘滞判定、超时判定用同一个时间基准。
    const auto now = std::chrono::steady_clock::now();

    bool eof = false;
    drainInput(now, eof);

    // 即使本帧一个字节都没读到也要结算悬空前缀，否则「按一下 ESC 然后不动」永远
    // 等不到它的 Quit。
    input_.tick(now);

    // buttons 完全覆盖（契约见 platform/window.hpp）。
    input_.fillButtonState(now, buttons);
    const auto pushToggle = [&events](render::InputEvent::Type type) {
        events.push_back(render::InputEvent{type, 0, 0});
    };
    if (buttons.isDown(render::Button::ToggleWireframe) &&
        !previousButtons_.isDown(render::Button::ToggleWireframe))
        pushToggle(render::InputEvent::Type::ToggleWireframe);
    if (buttons.isDown(render::Button::ToggleCulling) &&
        !previousButtons_.isDown(render::Button::ToggleCulling))
        pushToggle(render::InputEvent::Type::ToggleCulling);
    previousButtons_ = buttons;

    // 尺寸复查：以 ioctl 为准，信号只作提示。只有真的变了才追加 —— events 是边沿
    // 事件，重复的 Resize 会让下游反复重建网格。
    //
    // 这里无条件查询，而不是「只在信号到达时查询」，原因是信号可能丢失、被别的代码
    // 抢先处置或被屏蔽，把它当门控会漏掉 resize；ioctl 一次只要约 1µs，不值得省。
    // 标志在此消费掉：它存在的意义是给「将来改成带超时阻塞等待的做法」留一个唤醒源
    // （阻塞在 poll(2)/read(2) 上时，没有它就会漏 resize）—— 因此它只置位、不分支。
    g_sigwinchPending = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    if (querySize(w, h) && (w != width_ || h != height_))
    {
        width_ = w;
        height_ = h;
        events.push_back(render::InputEvent{render::InputEvent::Type::Resize, w, h});
    }

    if (eof)
        return false;
    return !buttons.isDown(render::Button::Quit);
}

void TtyWindow::drainInput(std::chrono::steady_clock::time_point now, bool &eof)
{
    char buffer[256];
    for (;;)
    {
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        const int ready = ::poll(&pfd, 1, 0);   // 零超时：本函数绝不阻塞主循环
        if (ready == 0)
            return;   // 本帧暂无数据 —— 正常情况，**不是** EOF
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            eof = true;
            return;
        }
        if ((pfd.revents & POLLIN) == 0)
        {
            // POLLHUP / POLLERR / POLLNVAL：终端（pty master）已经消失。
            eof = true;
            return;
        }

        const ssize_t n = ::read(fd_, buffer, sizeof buffer);
        if (n > 0)
        {
            input_.feed(buffer, static_cast<size_t>(n), now);
            continue;   // 继续排空：同一帧内的长按序列不该漏到下帧
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // poll 与 read 之间的竞态（别的读者抢先取走）：本帧按读完处理，
            // 既不是错误也不是 EOF。
            return;
        }
        // n == 0：POLLIN 就绪却读到 0 字节。VMIN=0 下这只能是真 EOF —— 这正是
        // 「先 poll 再 read」换来的区分能力。
        eof = true;
        return;
    }
}

bool TtyWindow::querySize(uint32_t &outWidth, uint32_t &outHeight) const noexcept
{
    struct winsize size{};
    if (::ioctl(fd_, TIOCGWINSZ, &size) != 0)
        return false;
    // 窗口最小化 / resize 过程中有些终端返回 0 行列。当作查询失败处理，保留上一次
    // 的有效值，免得下游拿 0 去算 aspect。
    if (size.ws_col == 0 || size.ws_row == 0)
        return false;
    outWidth = size.ws_col;
    outHeight = size.ws_row;
    return true;
}

const char *TtyWindow::name() const noexcept
{
    return "tty";
}

} // namespace my3d::platform
