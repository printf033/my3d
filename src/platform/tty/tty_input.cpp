#include "platform/tty/tty_input.hpp"

namespace my3d::platform
{
namespace
{

// 普通字符 → 语义按键。返回 Button::Count 表示「这个字节没绑定」。
//
// 只映射小写：终端发来的是 Shift 后的实际字符（'W' 不是 'w'），把大写也映射一遍
// 会让「左手小指误触 Shift」变成静默的不同动作；未绑定的字节一律忽略。
render::Button buttonForByte(uint8_t byte) noexcept
{
    switch (byte)
    {
    case 'w': return render::Button::MoveForward;
    case 's': return render::Button::MoveBackward;
    case 'a': return render::Button::MoveLeft;
    case 'd': return render::Button::MoveRight;
    case 'q': return render::Button::MoveDown;
    case 'e': return render::Button::MoveUp;
    case 'h': return render::Button::LookLeft;
    case 'l': return render::Button::LookRight;
    case 'j': return render::Button::LookDown;
    case 'k': return render::Button::LookUp;
    case 'f': return render::Button::ToggleWireframe;
    case 'c': return render::Button::ToggleCulling;
    default: return render::Button::Count;
    }
}

constexpr bool isCsiParamByte(uint8_t byte) noexcept
{
    // ECMA-48：参数字节 0x30–0x3F，中间字节 0x20–0x2F；两者并集是 0x20–0x3F。
    return byte >= 0x20 && byte <= 0x3f;
}

constexpr bool isCsiFinalByte(uint8_t byte) noexcept
{
    // final byte 0x40–0x7E，与参数区间不重叠，所以「参数」与「终结」不会混淆。
    return byte >= 0x40 && byte <= 0x7e;
}

} // namespace

void TtyInput::consume(uint8_t byte, TimePoint now, std::vector<render::Button> &out)
{
    switch (state_)
    {
    case State::Ground:
        if (byte == 0x1b)
        {
            state_ = State::Escape;   // 是独键还是序列开头，此刻无法判定
            return;
        }
        if (const render::Button b = buttonForByte(byte); b != render::Button::Count)
            emit(b, now, out);
        return;   // 其余字节（UTF-8、功能键尾巴等）静默忽略

    case State::Escape:
        if (byte == 0x1b)
        {
            // 连来两个 ESC：第一个已经确定是「单独 ESC」（第二个字节不属于它），
            // 当场结算为 Quit；第二个 ESC 当作新前缀的开头 —— 于是连按 ESC 会
            // 逐次退出，而不是被当成一个未知序列吃掉。
            emit(render::Button::Quit, now, out);
            state_ = State::Escape;
            return;
        }
        if (byte == '[' || byte == 'O')
        {
            // CSI / SS3 终端序列均消费至 final byte，不把方向键等别名映射为操作。
            state_ = State::Csi;
            paramCount_ = 0;
            return;
        }
        // ESC + 任意其它字节 = Alt+键 一类未绑定的序列。整段丢弃（两个字节都吃掉）：
        // 只丢 ESC 而让后续字符继续走 Ground，会让 Alt+w 变成 MoveForward ——
        // 即「未识别序列只丢自己，绝不能污染后面的字节」反过来被违反。
        state_ = State::Ground;
        return;

    case State::Csi:
        if (isCsiFinalByte(byte))
        {
            // 转义序列（包括方向键）不映射成应用按键；消费完整序列后回到 Ground，
            // 后续普通字符仍可正常解析。
            state_ = State::Ground;
            paramCount_ = 0;
            return;
        }
        if (isCsiParamByte(byte))
        {
            if (paramCount_ < kMaxParamBytes)
                ++paramCount_;
            // 超上限后不再计数但仍继续吞字节，直到 final byte，确保状态机收敛回
            // Ground。
            return;
        }
        // 控制字节出现在序列中间：序列已损坏，丢弃前缀，并把该字节按普通输入重新
        // 处理，确保解析状态不会卡住。
        state_ = State::Ground;
        paramCount_ = 0;
        consume(byte, now, out);   // 递归深度最多 2（Ground 分支不再递归）
        return;
    }
}

void TtyInput::emit(render::Button button, TimePoint now, std::vector<render::Button> &out)
{
    // 每次触发都把释放期限往后推：auto-repeat 期间期限不断续期，于是重复间隔
    // 小于 sticky_ 时「按下」状态连续成立。这一点是长按手感的关键。
    releaseAt_[static_cast<size_t>(button)] = now + sticky_;
    out.push_back(button);
}

void TtyInput::resetPending() noexcept
{
    state_ = State::Ground;
    paramCount_ = 0;
}

std::vector<render::Button> TtyInput::feed(const char *bytes, size_t count, TimePoint now)
{
    std::vector<render::Button> out;
    for (size_t i = 0; i < count; ++i)
    {
        lastByteTime_ = now;
        consume(static_cast<uint8_t>(bytes[i]), now, out);
    }
    return out;
}

std::vector<render::Button> TtyInput::tick(TimePoint now)
{
    std::vector<render::Button> out;
    if (state_ == State::Ground || now - lastByteTime_ < escapeTimeout_)
        return out;

    // 前缀静默超时。Escape 态意味着这一下就是单独的 ESC → Quit —— 这正是「单独按
    // ESC」与「ESC 是序列开头」的判定依据（见头文件第 2 条）。
    // Csi 态意味着序列被截断（对端消失、键盘固件异常）：整段丢弃，既不产生按键
    // 也不留下残状态，免得污染下一个字节。
    if (state_ == State::Escape)
        emit(render::Button::Quit, now, out);
    resetPending();
    return out;
}

void TtyInput::fillButtonState(TimePoint now, render::ButtonState &out) const noexcept
{
    // 逐键重算而非在旧值上叠加：契约要求完全覆盖（platform/window.hpp）。
    for (size_t i = 0; i < render::kButtonCount; ++i)
        out.down[i] = releaseAt_[i] > now;
}

void TtyInput::reset() noexcept
{
    resetPending();
    releaseAt_.fill(TimePoint{});
    lastByteTime_ = TimePoint{};
}

} // namespace my3d::platform
