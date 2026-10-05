// samples/viewer_gpu.cpp
//
// 新架构的第三个端到端示例，也是**验收 GPU 后端的那一个**：真实模型 →
// asset 导入 → scene::World → Filament（Vulkan）→ 屏幕 / 图片。
//
// 与 viewer_asset 的区别只有一处：把 AsciiRenderer 换成 FilamentRenderer。
// 前四步一字不差 —— 这正是新架构要证明的事：
//
//     assets/model/*.fbx|glb|gltf
//         → asset::importScene()          （assimp + stb，全项目唯一碰它们的文件）
//         → asset::AssetScene             （纯数据，无句柄）
//         → render::buildWorldFromAsset() （装配：建后端资源 + 建场景图）
//         → scene::World                  （IRenderer::beginFrame 的主语）
//         → backend::filament::FilamentRenderer
//
// 同一个 World 既喂过字符终端也喂 GPU，中间没有 #ifdef、没有"GPU 专用的场景
// 构建路径"—— 场景装配代码属于 my3d_core，它不知道后端是谁。这是本示例存在
// 的全部意义，不是顺带的性质。
//
// ============================ 两条输出路径 ============================
//
//   * 默认（有窗口）：SdlWindow → FilamentRenderer::attachSwapChain() → 直接上屏。
//   * --headless    ：不开窗口，attachOffscreen() 渲到 RenderTarget，
//                     readbackImage() 取回像素，编码成 PPM 落盘。
//
// headless 这条不是"调试开关"，是**验收手段**：屏幕截图无法被 diff、无法在
// CI 里断言，而一张写出来的图可以。它同时是唯一能在没有 X11/XWayland 的机器
// 上跑通的路径。
//
// ============================ 两条必须知道的坑 ============================
//
//  1. **SDL 必须跑在 X11 驱动上。** Filament 的 VulkanPlatformLinux 只认
//     X11 Window，拿到 Wayland 的 wl_surface 会直接失败。而本机
//     XDG_SESSION_TYPE=wayland、SDL 默认会选 wayland 驱动 —— 于是"能开窗口"
//     和"Filament 能画"成了两件事。这里在构造窗口**之前**强制
//     SDL_VIDEODRIVER=x11（XWayland 在 DISPLAY=:0 上兜底），这是环境适配，
//     不是设计偏好。
//
//  2. **readbackImage 回读的是线性值。** FilamentRenderer 用
//     setPostProcessingEnabled(false) 关掉了 tonemap/OETF，所以像素是场景线性
//     亮度。直接当 sRGB 写进图片会明显偏暗（0.2 线性 ≈ 0.48 sRGB，差一倍多）。
//     见下面 writePpm 的 linearToSrgb —— 那一步是**必需的编码**，不是可选的
//     "调色"。
//
// 用法：
//     viewer_gpu [options] [model-path]
//
//     --headless            离屏渲染出图，不开窗口
//     --size WxH            分辨率（默认 1280x720）
//     --frames N            headless 渲染帧数（默认 8）
//     --out PATH            headless 输出文件（默认 '<仓库根>/build/viewer_gpu.ppm'）
//     --ibl PATH            加载 IBL（.ktx / .ktx2，相对资产根）
//     --skybox PATH         加载天空盒（.ktx / .ktx2，相对资产根）
//     --no-environment      不加载任何环境（只靠方向光）
//     --unlit               无光照材质（保留 base-color 贴图），用于对比光照效果
//     --animation NAME      按名称播放骨骼动画；省略时播放第一条
//     -h, --help
//
//     环境变量 MY3D_LOG_LEVEL = trace|debug|info|warn|error|fatal|off

#include "ascii_demo_common.hpp"

#include "asset/ibl.hpp"
#include "asset/image_data.hpp"
#include "asset/importer.hpp"
#include "backend/filament/filament_renderer.hpp"
#include "platform/sdl/sdl_window.hpp"
#include "render/animation_driver.hpp"
#include "render/scene_builder.hpp"
#include "scene/world.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using namespace my3d;
using namespace my3d::demo;

// 仓库根。由 CMake 注入（见 samples/CMakeLists.txt），是 filamat / ktx / 默认
// 模型这些相对路径的解析根。运行时 cwd 不一定是仓库根（从 build/ 里启动、或
// IDE 指定别的工作目录），所以不能在运行时靠 relative path 猜。
//
// 只钉默认值：命令行显式给出的路径（--ibl / --skybox / model-path）仍按 cwd
// 解析 —— 那是用户自己的语义，不该被改。
#ifndef MY3D_ASSET_ROOT
#define MY3D_ASSET_ROOT "."
#endif

// 默认模型与 viewer_asset 保持一致：car 是 11 MB 的中档资产，纹理齐全、轮廓
// 认得出，GPU 后端上跑它是毫秒级；alienCIty / buildings 是几十 MB 的整条街区，
// 拿来压测更合适，不适合当默认。
constexpr const char *kDefaultModel = MY3D_ASSET_ROOT "/assets/model/car/FINAL_MODEL_B.fbx";

// 取同目录下的兄弟文件：'a/b/c.ktx' + 'sh.txt' -> 'a/b/sh.txt'。
// cmgen 把辐照度球谐写在 ibl.ktx 旁边，两者天然同目录。
std::string siblingPath(const std::string &path, const char *sibling)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return std::string(sibling);
    return path.substr(0, slash + 1) + sibling;
}

// 导入期把贴图压到这个边长以内。GPU 后端**确实会采样**这些贴图（不像 ASCII
// 后端只用 factor），所以这个值直接决定画质。512 是显存与清晰度的折中；
// 想看全画质传 --help 里没写的 0 需要改代码，这里先保持与 viewer_asset 同值，
// 好让两个示例的画面差异只来自"谁来画"。
constexpr uint32_t kMaxTextureSize = 512;

// 鼠标灵敏度（弧度/像素）。CameraController 把它私有化了（没有 getter），
// 所以这里自己存一份常量并同时用于 setLookSpeed —— 两处必须是同一个值，
// 写死在代码里比再给控制器加一个 getter 更不容易漂。
constexpr float kLookSpeed = 0.0025f;

// 滚轮一格换算成多少 FOV 增量（CameraAction 的 fovDelta 是任意单位，
// 由 CameraController 乘 fovSpeed_ * dt 得到弧度；一格滚轮 ≈ 4°，够细）。
constexpr float kWheelToFov = 3.0f;

struct Options
{
    std::string modelPath = kDefaultModel;
    std::string outPath;
    std::string iblPath;
    std::string skyboxPath;
    std::string animationName;
    uint32_t width = 1280;
    uint32_t height = 720;
    int frames = 8;
    bool headless = false;
    bool noEnvironment = false;
    bool unlit = false;
};

// 按模型的世界包围盒自动取景。
//
// 与 viewer_asset.cpp 里的同名函数是同一套逻辑（那边解释过为什么不能写死相机
// 位置：导入模型的尺度是未知量）。这里重复一份而不是抽公共头，是因为 GPU 后端
// 对 near/far 的要求更紧 —— 深度缓冲精度比软件光栅敏感，先把两边的取值分开
// 观察，确认稳定后再合并。重复 30 行换取"改动 A 不会静默影响已验证的 B"。
void frameCamera(scene::Camera &camera, const math::AABB &bounds)
{
    if (!bounds.valid)
    {
        LOG_WARN("asset scene has no geometry bounds; falling back to a default camera");
        camera = scene::Camera::lookingAt({3.4f, 2.6f, 4.6f}, {0.0f, 0.6f, 0.0f});
        return;
    }

    const math::Vec3 center = bounds.center();
    const float radius = std::max(math::length(bounds.extent()), 1e-4f);
    const float distance = radius / std::sin(camera.fovYRadians * 0.5f) * 1.15f;
    const math::Vec3 direction = math::normalize(math::Vec3{0.55f, 0.42f, 0.72f});

    camera = scene::Camera::lookingAt(center + direction * distance, center);
    camera.nearPlane = std::max(radius * 0.01f, 1e-3f);
    camera.farPlane = distance + radius * 4.0f;
}

// ---------------------------------------------------------------------------
// 线性 → sRGB。见文件头第 2 条坑：这一步不能省。
//
// 用的是 IEC 61966-2-1 的分段函数（不是 pow(x, 1/2.2) 的近似）：低亮度段线性
// 能保住暗部的层次，近似公式会在近黑处把细节抹掉，而 car 这种深色模型正好
// 大量落在那一档。
// ---------------------------------------------------------------------------
inline float linearToSrgb(float c)
{
    if (c <= 0.0f)
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return c <= 0.0031308f ? 12.92f * c
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline uint8_t toByte(float linear)
{
    return static_cast<uint8_t>(linearToSrgb(linear) * 255.0f + 0.5f);
}

// 写二进制 PPM（P6）。
//
// 为什么是 PPM 而不是 PNG：仓库里只有 stb_image.h（读），没有 stb_image_write.h
// （写），而新增一个第三方头要用户批准 —— 为了一张验收图不值得。PPM 零依赖、
// 无损、格式简单到不可能写错，缺点只是文件大（1280x720 ≈ 2.7 MB）和需要外部
// 工具转格式。验收目标是"证明真的画出来了"，后者由文件存在 + 像素非零 + 肉眼
// 可看三条覆盖，够了。
//
// 行序直传：ImageData 的约定是"第 0 行在顶"，readbackImage 的注释里说明了
// 为什么 Filament 的离屏回读也满足这个约定。PPM 同样是"第 0 行在顶"，所以
// 这里不翻转 —— 如果图上下颠倒了，那是上游的约定破了，不该在这里遮住。
bool writePpm(const std::string &path, const asset::ImageData &image)
{
    if (!image.valid())
    {
        LOG_ERROR("readback produced an invalid image ({}x{})", image.width, image.height);
        return false;
    }

    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
        LOG_ERROR("cannot open '{}' for writing: {}", path, std::strerror(errno));
        return false;
    }

    std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);

    // 编码到 sRGB 后写。colorSpace 已经是 Linear 是 Filament 后端的既有行为
    // （setPostProcessingEnabled(false)），这里仍然判断一次：将来后端加了
    // tonemap，回读会变成 sRGB，这一行能让输出自动跟上而不是解两次 gamma。
    const bool encode = (image.colorSpace == asset::ColorSpace::Linear);

    std::vector<uint8_t> row(static_cast<size_t>(image.width) * 3u);
    for (uint32_t y = 0; y < image.height; ++y)
    {
        const uint8_t *src = image.pixels.data() + static_cast<size_t>(y) * image.width * 4u;
        for (uint32_t x = 0; x < image.width; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                const float v = static_cast<float>(src[x * 4u + static_cast<size_t>(c)]) / 255.0f;
                row[x * 3u + static_cast<size_t>(c)] = encode ? toByte(v) : src[x * 4u + static_cast<size_t>(c)];
            }
        }
        if (std::fwrite(row.data(), 1, row.size(), file) != row.size())
        {
            LOG_ERROR("short write to '{}'", path);
            std::fclose(file);
            return false;
        }
    }

    if (std::fclose(file) != 0)
    {
        LOG_ERROR("cannot flush '{}': {}", path, std::strerror(errno));
        return false;
    }
    return true;
}

void printUsage(const char *program)
{
    std::fprintf(stderr,
                 "usage: %s [options] [model-path]\n"
                 "\n"
                 "  model-path        .fbx / .glb / .gltf 等 assimp 支持的格式\n"
                 "                    省略时用 %s\n"
                 "\n"
                 "  --headless        离屏渲染出图，不开窗口（无需 X11）\n"
                 "  --size WxH        分辨率，默认 1280x720\n"
                 "  --frames N        headless 渲染帧数，默认 8\n"
                 "  --out PATH        headless 输出文件，默认 '<仓库根>/build/viewer_gpu.ppm'\n"
                 "  --ibl PATH        IBL 反射/辐照度（.ktx / .ktx2，相对资产根）\n"
                 "  --skybox PATH     天空盒（.ktx / .ktx2，相对资产根）\n"
                 "  --no-environment  不加载环境，只靠方向光\n"
                 "  --unlit           无光照材质（保留 base-color 贴图）\n"
                 "  --animation NAME  按名称播放骨骼动画；省略时播放第一条\n"
                 "  -h, --help\n"
                 "\n"
                 "  环境变量 MY3D_LOG_LEVEL = trace|debug|info|warn|error|fatal|off\n"
                 "\n"
                 "仓库里可用的环境贴图：\n"
                 "  assets/IBL/sky/cmgen/ibl.ktx          cmgen 产物（KTX1）\n"
                 "  assets/IBL/sky/cmgen/skybox.ktx       cmgen 产物（KTX1）\n"
                 "  assets/IBL/sky/toktx/ibl.ktx2         toktx 产物（KTX2）\n"
                 "  assets/IBL/sky/toktx/skybox.ktx2      toktx 产物（KTX2）\n",
                 program, kDefaultModel);
}

bool parseSize(const std::string &text, uint32_t &width, uint32_t &height)
{
    const size_t x = text.find_first_of("xX");
    if (x == std::string::npos)
        return false;
    try
    {
        const long w = std::stol(text.substr(0, x));
        const long h = std::stol(text.substr(x + 1));
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return false;
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 返回值：0 = 继续（解析成功），1 = 已处理（--help），2 = 错误。
int parseArgs(int argc, char **argv, Options &options)
{
    int positional = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};

        auto next = [&](const char *name, std::string &out) -> bool
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "viewer_gpu: %s needs a value\n", name);
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            return 1;
        }
        if (arg == "--headless")
        {
            options.headless = true;
        }
        else if (arg == "--no-environment")
        {
            options.noEnvironment = true;
        }
        else if (arg == "--unlit")
        {
            options.unlit = true;
        }
        else if (arg == "--size")
        {
            std::string value;
            if (!next("--size", value))
                return 2;
            if (!parseSize(value, options.width, options.height))
            {
                std::fprintf(stderr, "viewer_gpu: bad --size '%s' (want e.g. 1280x720)\n", value.c_str());
                return 2;
            }
        }
        else if (arg == "--frames")
        {
            std::string value;
            if (!next("--frames", value))
                return 2;
            try
            {
                options.frames = std::max(1, std::stoi(value));
            }
            catch (const std::exception &)
            {
                std::fprintf(stderr, "viewer_gpu: bad --frames '%s'\n", value.c_str());
                return 2;
            }
        }
        else if (arg == "--out")
        {
            if (!next("--out", options.outPath))
                return 2;
        }
        else if (arg == "--ibl")
        {
            if (!next("--ibl", options.iblPath))
                return 2;
        }
        else if (arg == "--skybox")
        {
            if (!next("--skybox", options.skyboxPath))
                return 2;
        }
        else if (arg == "--animation")
        {
            if (!next("--animation", options.animationName))
                return 2;
            if (options.animationName.empty())
            {
                std::fprintf(stderr, "viewer_gpu: --animation needs a clip name\n");
                return 2;
            }
        }
        else if (!arg.empty() && arg.front() == '-')
        {
            std::fprintf(stderr, "viewer_gpu: unknown option '%.*s'\n", (int)arg.size(), arg.data());
            printUsage(argv[0]);
            return 2;
        }
        else
        {
            if (++positional > 1)
            {
                std::fprintf(stderr, "viewer_gpu: only one model path is accepted\n");
                printUsage(argv[0]);
                return 2;
            }
            options.modelPath = std::string{arg};
        }
    }
    return 0;
}

// headless：离屏渲 N 帧 → 回读 → 写 PPM。
bool applyAnimation(render::AnimationDriver &driver, scene::World &world,
                    backend::filament::FilamentRenderer &renderer,
                    const std::vector<MeshHandle> &meshHandles, float timeSeconds)
{
    if (!driver.bound())
        return true;
    if (!driver.apply(timeSeconds, world, renderer, meshHandles))
    {
        LOG_ERROR("animation update failed: {}", driver.error());
        return false;
    }

    const render::AnimationDriver::Stats &stats = driver.stats();
    if (stats.failedUploads > 0)
    {
        LOG_ERROR("animation update failed to upload {} mesh(es)", stats.failedUploads);
        return false;
    }
    return true;
}

int runHeadless(backend::filament::FilamentRenderer &renderer, scene::World &world,
                render::CameraController &controller, const Options &options,
                render::AnimationDriver &animation,
                const std::vector<MeshHandle> &meshHandles)
{
    if (!renderer.attachOffscreen(options.width, options.height))
    {
        LOG_ERROR("attachOffscreen failed ({}x{})", options.width, options.height);
        return 1;
    }

    render::View view;
    view.viewport = {options.width, options.height};
    view.camera = controller.camera();
    view.settings.wireframe = false;
    view.settings.enableCulling = true;
    view.settings.enableDepthTest = true;

    // 多渲几帧：Filament 的首帧要建管线、上传纹理、编译着色器，把它算进
    // 画面里会得到一张"材质还没就绪"的图。帧数由 --frames 控制，默认 8 足够
    // 走完冷启动；有动画时每帧按采样时刻更新姿势。
    for (int i = 0; i < options.frames; ++i)
    {
        const double timeSeconds = static_cast<double>(i) / 60.0;
        if (!applyAnimation(animation, world, renderer, meshHandles,
                            static_cast<float>(timeSeconds)))
            return 1;
        const render::FrameInfo info{&world, &view, timeSeconds};
        if (!renderer.beginFrame(info))
        {
            LOG_ERROR("beginFrame failed at frame {}", i);
            return 1;
        }
        renderer.endFrame();
    }

    asset::ImageData image;
    if (!renderer.readbackImage(image))
    {
        LOG_ERROR("readbackImage failed");
        return 1;
    }

    // 「画出来了没有」的判据：与清屏色**不同**的像素数。
    //
    // 只统计「非黑」会骗人：空的离屏目标回读出来是清屏色，它既不是黑、又占满
    // 全图，会把「Scene 里一个实体都没有」报成 100% 成功。
    //
    // 还有一个更安静的坑：image.pixels 是**线性**值（writePpm 里才做 sRGB
    // 编码），所以参考值必须取线性字节。拿 toByte() 得到的 sRGB 编码值
    // (64,64,75) 去和线性空间的 (13,13,19) 比，会把整个背景都算成「画了东西」
    // —— 这个错版判据刚被实测抓到过（背景占 90% 却报 100% drawn）。
    const auto linByte = [](float c)
    { return static_cast<uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };

    const uint8_t clearR = linByte(view.settings.clearColor.x);
    const uint8_t clearG = linByte(view.settings.clearColor.y);
    const uint8_t clearB = linByte(view.settings.clearColor.z);

    size_t drawn = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
    {
        const uint32_t r = image.pixels[i], g = image.pixels[i + 1], b = image.pixels[i + 2];
        sum += r + g + b;
        // ±3 是线性↔sRGB 往返加 8bit 量化的裕量；超过它才算「画了东西」。
        if (std::abs(static_cast<int>(r) - static_cast<int>(clearR)) > 3 ||
            std::abs(static_cast<int>(g) - static_cast<int>(clearG)) > 3 ||
            std::abs(static_cast<int>(b) - static_cast<int>(clearB)) > 3)
            ++drawn;
    }
    const size_t total = static_cast<size_t>(image.width) * image.height;
    const double drawnPct = total ? 100.0 * static_cast<double>(drawn) / static_cast<double>(total) : 0.0;
    LOG_INFO("readback {}x{}: clear linear=({},{},{}), {}/{} pixels differ from clear ({:.2f}%), mean linear {:.4f}",
             image.width, image.height, clearR, clearG, clearB,
             drawn, total, drawnPct,
             total ? static_cast<double>(sum) / (3.0 * static_cast<double>(total) * 255.0) : 0.0);

    if (drawn == 0)
    {
        LOG_ERROR("nothing was drawn: every pixel still equals the clear color "
                  "(empty Scene, camera pointing at nothing, or geometry never registered)");
        return 1;
    }

    // 默认输出同样以仓库根为根：headless 的典型用法是从 build/ 里启动，
    // 裸相对路径会落到 build/build/ 上。
    const std::string outPath =
        options.outPath.empty() ? MY3D_ASSET_ROOT "/build/viewer_gpu.ppm" : options.outPath;
    if (!writePpm(outPath, image))
        return 1;

    std::fprintf(stderr, "viewer_gpu: wrote %s (%ux%u)\n", outPath.c_str(), image.width, image.height);
    return 0;
}

// 有窗口：SDL（X11）→ Filament swapchain → 上屏。ESC / 关窗退出。
int runWindowed(backend::filament::FilamentRenderer &renderer, scene::World &world,
                render::CameraController &controller, const Options &options,
                render::AnimationDriver &animation,
                const std::vector<MeshHandle> &meshHandles)
{
    // 见文件头第 1 条坑。必须在 SdlWindow 构造（它调 SDL_Init）之前设。
    // override=1 覆盖已有值：继承来的空值或别的驱动都不该赢过这条硬约束。
    ::setenv("SDL_VIDEODRIVER", "x11", 1);

    platform::SdlWindow window;

    void *nativeWindow = window.nativeWindowHandle();
    if (!nativeWindow)
    {
        LOG_ERROR("SDL window has no native handle (WM info unavailable); "
                  "Filament cannot create a swap chain on it");
        return 1;
    }
    if (!renderer.attachSwapChain(nativeWindow))
    {
        LOG_ERROR("attachSwapChain failed");
        return 1;
    }

    renderer.resize(window.width(), window.height());

    render::View view;
    view.viewport = {window.width(), window.height()};
    view.settings.wireframe = false;
    view.settings.enableCulling = true;
    view.settings.enableDepthTest = true;

    LOG_INFO("viewer_gpu: window {}x{} px, driver x11", window.width(), window.height());

    render::ButtonState buttons;
    std::vector<render::InputEvent> events;

    constexpr double kTargetFrameSeconds = 1.0 / 60.0;

    auto previous = std::chrono::steady_clock::now();
    double timeSeconds = 0.0;
    uint64_t frames = 0;

    while (window.poll(buttons, events))
    {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        timeSeconds += dt;

        for (const render::InputEvent &event : events)
        {
            switch (event.type)
            {
            case render::InputEvent::Type::Resize:
                renderer.resize(event.width, event.height);
                view.viewport = {event.width, event.height};
                break;
            case render::InputEvent::Type::ToggleWireframe:
                view.settings.wireframe = !view.settings.wireframe;
                LOG_INFO("wireframe = {}", view.settings.wireframe);
                break;
            case render::InputEvent::Type::ToggleCulling:
                view.settings.enableCulling = !view.settings.enableCulling;
                LOG_INFO("culling = {}", view.settings.enableCulling);
                break;
            }
        }
        events.clear();

        // 鼠标：窗口层给的是本帧像素增量（见 sdl_window.hpp 的契约），
        // CameraAction 的方向约定是"yawDelta > 0 = 右转"，屏幕 x 向右为正
        // 正好同号；y 向下为正而"抬头"是正 pitch，所以这里取负。
        render::CameraAction action = toAction(buttons);
        action.yawDelta += window.pointerDeltaX() * kLookSpeed;
        action.pitchDelta -= window.pointerDeltaY() * kLookSpeed;
        action.fovDelta += window.wheelDelta() * kWheelToFov;
        controller.apply(action, dt);

        if (!applyAnimation(animation, world, renderer, meshHandles,
                            static_cast<float>(timeSeconds)))
            return 1;

        view.camera = controller.camera();

        const render::FrameInfo info{&world, &view, timeSeconds};
        if (!renderer.beginFrame(info))
        {
            LOG_ERROR("beginFrame failed");
            break;
        }
        renderer.endFrame();

        ++frames;

        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
        if (spent < kTargetFrameSeconds)
            std::this_thread::sleep_for(std::chrono::duration<double>(kTargetFrameSeconds - spent));
    }

    LOG_INFO("viewer_gpu: {} frames, {} primitives in last frame, window {}x{}",
             frames, renderer.lastFramePrimitives(), options.width, options.height);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    configureLogging();

    Options options;
    switch (parseArgs(argc, argv, options))
    {
    case 1:
        return 0;
    case 2:
        return 2;
    default:
        break;
    }

    // ------------------------------------------------------------------
    // 导入。放在构造窗口 / Engine 之前：导入失败时不必先把 GPU 拉起来，
    // 错误也留在普通终端里（窗口模式没有终端接管问题，但顺序更省事）。
    // ------------------------------------------------------------------
    asset::ImportOptions importOptions;
    importOptions.maxTextureSize = kMaxTextureSize;

    LOG_INFO("importing '{}' ...", options.modelPath);

    const auto importStart = Clock::now();
    asset::ImportResult imported = asset::importScene(options.modelPath, importOptions);
    const double importSeconds = std::chrono::duration<double>(Clock::now() - importStart).count();

    if (!imported.ok)
    {
        LOG_ERROR("import failed: {}", imported.error);
        std::fprintf(stderr, "viewer_gpu: cannot import '%s'\n", options.modelPath.c_str());
        return 1;
    }

    LOG_INFO("imported in {:.2f}s: {} nodes, {} meshes, {} materials, {} images, "
             "{} vertices, {} triangles, {} animations",
             importSeconds,
             imported.stats.nodeCount, imported.stats.meshCount, imported.stats.materialCount,
             imported.stats.textureCount, imported.stats.vertexCount, imported.stats.triangleCount,
             imported.stats.animationCount);

    for (const std::string &warning : imported.warnings)
        LOG_WARN("import: {}", warning);

    if (options.unlit)
    {
        for (asset::MaterialDesc &material : imported.scene.materials)
            material.model = asset::ShadingModel::Unlit;
        LOG_INFO("unlit mode enabled: lighting is bypassed; base-color textures remain active");
    }

    // ------------------------------------------------------------------
    // GPU 后端。它同时是 IRenderDevice，所以必须在装配之前构造。
    //
    // 构造会创建 Filament Engine（Vulkan），失败抛 std::runtime_error ——
    // 没有 Vulkan 驱动的机器走到这里会得到一条明确的错误，而不是后续某个
    // 函数里的空指针解引用。
    // ------------------------------------------------------------------
    std::unique_ptr<backend::filament::FilamentRenderer> renderer;
    try
    {
        renderer = std::make_unique<backend::filament::FilamentRenderer>(MY3D_ASSET_ROOT);
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("cannot create the filament backend: {}", error.what());
        std::fprintf(stderr, "viewer_gpu: GPU backend unavailable (no Vulkan device?)\n");
        return 1;
    }

    scene::World world;
    const render::SceneBuildResult built =
        render::buildWorldFromAsset(imported.scene, *renderer, world);

    if (!built.ok)
    {
        LOG_ERROR("scene build failed: {}", built.error);
        std::fprintf(stderr, "viewer_gpu: cannot build scene from '%s'\n", options.modelPath.c_str());
        return 1;
    }

    for (const std::string &warning : built.warnings)
        LOG_WARN("build: {}", warning);

    render::AnimationDriver animation;
    if (imported.scene.animations.empty())
    {
        if (!options.animationName.empty())
        {
            LOG_ERROR("requested animation '{}' but the asset has no animations",
                      options.animationName);
            return 1;
        }
        LOG_INFO("no animation in this asset; showing the bind pose");
    }
    else
    {
        const bool bound = options.animationName.empty()
                               ? animation.bind(imported.scene, 0u)
                               : animation.bind(imported.scene, options.animationName);
        if (!bound)
        {
            if (!options.animationName.empty())
            {
                LOG_ERROR("animation bind failed: {}", animation.error());
                return 1;
            }
            LOG_WARN("animation bind failed: {}", animation.error());
        }
        else
        {
            const render::AnimationDriver::Stats &stats = animation.stats();
            LOG_INFO("animation '{}' bound: {:.2f}s, {}/{} channels resolved, {} skinned "
                     "mesh(es), {} vertices",
                     animation.clipName(), animation.duration(), stats.resolvedChannels,
                     stats.clipChannels, stats.skinnedMeshes, stats.skinnedVertices);

            if (!applyAnimation(animation, world, *renderer, built.meshHandles, 0.0f))
                return 1;
        }
    }

    LOG_INFO("built: {} nodes, {} primitives, {} meshes, {} materials{}",
             built.stats.nodes, built.stats.primitives, built.stats.meshes, built.stats.materials,
             built.stats.skippedPrimitives ? " (+" + std::to_string(built.stats.skippedPrimitives) +
                                                 " skipped)" : "");

    // ------------------------------------------------------------------
    // 环境。**必须在 build 之后**：buildWorldFromAsset 是覆盖语义，它调用的
    // World::clear() 会把 lights 和 environment 一起清掉。
    //
    // IBL 的加载失败只告警不中断：没有环境的画面是"暗"，不是"错"，而
    // 用户跑这个示例的首要目的是确认几何/材质/相机对了。把可选资源缺失升级成
    // 致命错误，会让一个能出图的构建看起来像坏的。
    // ------------------------------------------------------------------
    if (!options.noEnvironment)
    {
        if (!options.iblPath.empty())
        {
            const IblHandle ibl = renderer->loadIblFromKtx(options.iblPath);
            if (ibl)
            {
                world.environment().ibl = ibl;
                LOG_INFO("ibl loaded from '{}'", options.iblPath);

                // cmgen 把漫反射辐照度单独写在同目录的 sh.txt（9 个球谐
                // 系数）。Filament 的 IndirectLight 是 reflections 贴图 +
                // 球谐两部分；只给贴图等于只加镜面反射，漫反射辐照度为零。
                // CPU / ASCII 后端读的是同一份数据（shading.cpp 的 shBands）。
                // 读不到不报错：降级成只有镜面反射。
                const std::string shPath = siblingPath(options.iblPath, "sh.txt");
                asset::SphericalHarmonics sh;
                if (asset::loadSphericalHarmonics(shPath, sh))
                {
                    for (size_t i = 0; i < 9; ++i)
                        world.environment().shBands[i] = sh.bands[i];
                    LOG_INFO("辐照度 sh loaded from '{}'", shPath);
                }
                else
                {
                    LOG_WARN("辐照度 sh '{}' 读取失败；只有镜面反射生效", shPath);
                }
            }
            else
            {
                LOG_WARN("cannot load ibl '{}'; continuing without reflections", options.iblPath);
            }
        }
        if (!options.skyboxPath.empty())
        {
            if (!renderer->setSkyboxFromKtx(options.skyboxPath))
                LOG_WARN("cannot load skybox '{}'; continuing without it", options.skyboxPath);
            else
                LOG_INFO("skybox loaded from '{}'", options.skyboxPath);
        }
    }
    else
    {
        world.environment().ibl = {};
    }

    // 两盏方向光：key 给形状，fill 给暗部一点冷色。强度值是 FILAMENT 侧的
    // 物理单位（lux），换算常数在 filament_renderer.cpp 的
    // kLuxPerIntensityUnit —— 那边从 1.0 起步定成 100000，所以这里 3.2 / 0.9
    // 落在"能看清"的区间。若画面整体过曝或过暗，改的是那个常数，不是这里。
    scene::LightDesc keyLight;
    keyLight.name = "key";
    keyLight.type = scene::LightType::Directional;
    keyLight.direction = {-0.45f, -1.0f, -0.35f};
    keyLight.color = {1.0f, 0.96f, 0.88f};
    // Filament 后端把方向光强度换算成 lux（×100000）。原先 3.2 对应
    // 320000 lux，令当前材质在显示变换前大面积裁白；这里用 3.2 lux，
    // 让 base-color 贴图的颜色保留下来。
    keyLight.intensity = 0.000032f;
    world.addLight(keyLight);

    scene::LightDesc fillLight;
    fillLight.name = "fill";
    fillLight.type = scene::LightType::Directional;
    fillLight.direction = {0.6f, -0.35f, 0.5f};
    fillLight.color = {0.35f, 0.42f, 0.6f};
    fillLight.intensity = 0.000009f;
    world.addLight(fillLight);

    math::AABB sceneBounds = world.worldBounds();
    if (animation.bound())
    {
        const std::vector<math::Mat4> &nodeWorld = world.worldMatrices();
        math::AABB animatedBounds;
        for (const render::AnimationDriver::MeshBounds &meshBounds :
             animation.meshBoundsDiag(world))
        {
            if (!meshBounds.box.valid || meshBounds.nodeIndex >= nodeWorld.size())
                continue;
            animatedBounds.expand(meshBounds.box.transformed(nodeWorld[meshBounds.nodeIndex]));
        }
        if (animatedBounds.valid)
            sceneBounds = animatedBounds;
    }
    if (sceneBounds.valid)
    {
        const math::Vec3 c = sceneBounds.center();
        const math::Vec3 e = sceneBounds.extent();
        LOG_INFO("world bounds center ({:.3f}, {:.3f}, {:.3f}), "
                 "half-extent ({:.3f}, {:.3f}, {:.3f})",
                 c.x, c.y, c.z, e.x, e.y, e.z);
    }

    // ------------------------------------------------------------------
    // 相机
    // ------------------------------------------------------------------
    render::CameraController controller;
    controller.camera() = scene::Camera{};
    frameCamera(controller.camera(), sceneBounds);

    const float radius = sceneBounds.valid ? math::length(sceneBounds.extent()) : 1.0f;
    controller.setMoveSpeed(std::max(radius * 1.2f, 0.1f));
    controller.setLookSpeed(kLookSpeed);

    {
        const math::Vec3 p = controller.camera().position;
        LOG_INFO("camera at ({:.3f}, {:.3f}, {:.3f}), near {:.4f}, far {:.1f}",
                 p.x, p.y, p.z, controller.camera().nearPlane, controller.camera().farPlane);
    }

    return options.headless
               ? runHeadless(*renderer, world, controller, options, animation, built.meshHandles)
               : runWindowed(*renderer, world, controller, options, animation, built.meshHandles);
}
