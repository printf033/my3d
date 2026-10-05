#pragma once

// my3d::render —— 语义输入。
//
// 平台层（SDL / TTY）把物理按键、鼠标、转义序列翻译成这里的语义量；app 层把
// 语义量翻译成 CameraAction。于是「按右键向右看」这类手感只有一份，换平台不改
// 手感（见 docs/architecture.md §5.7 / §6.5）。
//
// 刻意不并入 render/types.hpp：那是「这一帧要画什么」，这里是「用户想干什么」，
// 两者的变更原因不同 —— 混在一起会让 RenderSettings 慢慢长成交互配置的大杂烩。

#include <cstddef>
#include <cstdint>

namespace my3d::render
{

// 语义按键，与物理键位无关。
//
// TTY 的 h 与 SDL 的 H 都映射到 LookLeft，其它移动与视角键也逐一对应，所以
// 漫游手感在两个平台上一致 —— 这是把映射放在平台层、把数学放在 CameraController
// 的收益。
enum class Button : uint8_t
{
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    LookUp,
    LookDown,
    LookLeft,
    LookRight,
    Quit,
    ToggleWireframe,
    ToggleCulling,
    Count
};

inline constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);

// 持续量：本帧内该动作是否处于按下状态。
//
// 用固定数组而非 std::set：按键数是个封闭的小集合，数组无分配、可 constexpr、
// 且 isDown 是无分支的索引访问。
struct ButtonState
{
    bool down[kButtonCount]{};

    constexpr bool isDown(Button b) const noexcept { return down[static_cast<size_t>(b)]; }

    constexpr void set(Button b, bool value) noexcept { down[static_cast<size_t>(b)] = value; }

    constexpr void clear() noexcept
    {
        for (size_t i = 0; i < kButtonCount; ++i)
            down[i] = false;
    }
};

// 一次性事件（边沿触发）。平台层负责去抖与边沿判定，app 层直接消费。
struct InputEvent
{
    enum class Type : uint8_t
    {
        Resize,
        ToggleWireframe,
        ToggleCulling
    };

    Type type = Type::Resize;
    uint32_t width = 0;    // 仅 Resize 有效：新的列数 / 窗口宽
    uint32_t height = 0;   // 仅 Resize 有效：新的行数 / 窗口高
};

} // namespace my3d::render
