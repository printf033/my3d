#pragma once

// samples/ascii_demo_common.hpp —— 两个 ASCII 示例共用的一段接线。
//
// 为什么是头文件而不是各自的 .cpp 副本：libs/mylog 是一个仍在演进的上游
// （见 src/CMakeLists.txt 里「消费的是上游的头，不是上游的 CMake 接口」那段），
// 日志接线只应该有一个落点。复制的代价不是多几行，而是升级时改漏一处 —— 而
// 漏掉的那一处不会编译失败，只会让日志静默落到错误的 sink 上（默认 sink 是
// stdout，正好和终端交替屏幕协议的帧数据同流，画出一屏花）。
//
// 这里放的都是「与具体场景无关」的东西：日志、整帧写出、按键→相机动作。
// 场景怎么搭、模型怎么载，留在各自的 sample 里。
//
// 依赖：mylog（header-only）+ platform/window.hpp + render/camera_controller.hpp。
// 都是新架构内部的东西，不引入任何第三方目标 —— viewer_ascii 的链接判据
// （仅 libstdc++ / libgcc_s / libm / libc）因此不受影响。

#include "logger.hpp"
#include "platform/window.hpp"
#include "render/camera_controller.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace my3d::demo
{

using Clock = std::chrono::steady_clock;

// 语义按键 → CameraAction。这是「同一套手感，多种输入」里 app 层的那一步：
// 平台层只报「左转是按下的」，怎么换算成弧度由 CameraController 决定。
inline render::CameraAction toAction(const render::ButtonState &buttons)
{
    render::CameraAction action;
    if (buttons.isDown(render::Button::MoveForward))  action.forward += 1.0f;
    if (buttons.isDown(render::Button::MoveBackward)) action.forward -= 1.0f;
    if (buttons.isDown(render::Button::MoveRight))    action.strafe += 1.0f;
    if (buttons.isDown(render::Button::MoveLeft))     action.strafe -= 1.0f;
    if (buttons.isDown(render::Button::MoveUp))       action.lift += 1.0f;
    if (buttons.isDown(render::Button::MoveDown))     action.lift -= 1.0f;
    if (buttons.isDown(render::Button::LookRight))    action.yawDelta += 1.0f;
    if (buttons.isDown(render::Button::LookLeft))     action.yawDelta -= 1.0f;
    if (buttons.isDown(render::Button::LookUp))       action.pitchDelta += 1.0f;
    if (buttons.isDown(render::Button::LookDown))     action.pitchDelta -= 1.0f;
    return action;
}

// 整帧单次写完。分片写会让终端在中途看到半个转义序列 —— 终端的解析器确实跨
// write 保持状态，但中途多一次调度就多一次可见的撕裂。返回 false = 输出流没了。
inline bool writeAll(const std::string &bytes)
{
    size_t written = 0;
    while (written < bytes.size())
    {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 日志
//
// 上游 mylog 的默认 sink 是 **stdout**（`detail::stdoutOutput`）。而本程序的
// stdout 正被终端交替屏幕协议接管（`\x1b[?1049h`）—— 日志一旦落进去，就会和
// 帧数据同流，在终端上画出花屏。所以把 sink 换成 stderr 不是调优项，是这个程序
// 能跑起来的前提，和上面 writeAll 只写 stdout 是同一条边界的两侧。
//
// stderr 不参与交替屏幕缓冲，所以退出后统计仍然可见 —— 这一点与原有行为一致。
//
// 级别由环境变量 MY3D_LOG_LEVEL 控制（trace/debug/info/warn/error/fatal/off），
// 默认 info。调试渲染或输入问题时：MY3D_LOG_LEVEL=debug build/viewer_ascii。
// ---------------------------------------------------------------------------
inline void logToStderr(void * /*ctx*/, std::string_view message)
{
    if (!message.empty())
        std::fwrite(message.data(), 1, message.size(), stderr);
}

inline void flushStderr(void * /*ctx*/)
{
    std::fflush(stderr);
}

inline mylog::LEVEL levelFromEnv()
{
    const char *raw = std::getenv("MY3D_LOG_LEVEL");
    if (raw == nullptr)
        return mylog::LEVEL::INFO;

    const std::string_view wanted{raw};

    // 顺序必须与 mylog::LEVEL 的枚举值一致（TRACE=0 ... FATAL=5）。
    static constexpr std::string_view kNames[] = {
        "trace", "debug", "info", "warn", "error", "fatal",
    };

    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
    {
        if (wanted == kNames[i])
            return static_cast<mylog::LEVEL>(i);
    }

    // "off" 映到最高级别：抑制判定是 `level >= suppressLevel`，所以 FATAL 会把
    // TRACE..ERROR 全部吃掉。严格说并不是「全关」——FATAL 仍会出，但那本来就不该
    // 被安静掉。
    if (wanted == "off")
        return mylog::LEVEL::FATAL;

    std::fprintf(stderr,
                 "my3d: unknown MY3D_LOG_LEVEL '%s' "
                 "(want trace|debug|info|warn|error|fatal|off), using info\n",
                 raw);
    return mylog::LEVEL::INFO;
}

// 必须在任何输出之前调用：TtyWindow 一旦构造就进入交替屏幕，此后 stdout 不再是
// 一个可以随便写的地方。
inline void configureLogging()
{
    mylog::Logger &logger = mylog::Logger::getInstance();

    // 先设级别、再发配置：让第一条日志就已经落在正确的级别与 sink 上。
    logger.setSuppressLevel(levelFromEnv());

    mylog::Logger::Config config = logger.getConfig();
    config.outputFunction = mylog::OutputFunction{&logToStderr, nullptr};
    config.flushFunction = mylog::FlushFunction{&flushStderr, nullptr};
    logger.setConfig(config);
}

} // namespace my3d::demo
