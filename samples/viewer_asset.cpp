// samples/viewer_asset.cpp
//
// 新架构的第二个端到端示例：**真实模型** → asset 导入 → scene::World → 字符终端。
//
// 与 viewer_ascii 的唯一区别是场景从哪来：那边是程序化几何（makeCube/makeGround），
// 这边走完整的一条链
//
//     assets/model/*.fbx|glb|gltf
//         → asset::importScene()          （assimp + stb，全项目唯一碰它们的文件）
//         → asset::AssetScene             （纯数据，无句柄）
//         → render::buildWorldFromAsset() （装配：建后端资源 + 建场景图）
//         → scene::World                  （IRenderer::beginFrame 的主语）
//         → backend::ascii::AsciiRenderer
//
// 中间那段 buildWorldFromAsset 就是这张图里此前缺的一环：AssetScene 一直没有
// 消费者，接缝留在 sample 里。它现在在 render/ 层，所以下一个后端（Filament /
// Vulkan）无需重写这段装配 —— 换掉 device 参数即可。
//
// ============================ 为什么是独立可执行文件 ============================
//
// 本 target 链 my3d_asset_import，因而间接链上 libassimp.so —— `ldd` 的输出里
// 会出现第三方库。viewer_ascii 的验收判据是「ldd 只有 libstdc++ / libgcc_s /
// libm / libc」（docs/architecture.md §8.1、samples/CMakeLists.txt），把模型加载
// 并进那个 target 会在构建期把这条判据毁掉，而且毁得没有声音 —— 判据本身不会
// 报错，它只是从此永远为真或永远为假。
//
// 所以：viewer_ascii 继续是「引擎 API 不拖第三方」的证据，viewer_asset 是
// 「导入器接进来之后引擎照样跑」的证据。两个可执行文件各证一件事，链接清单
// 就是证据本身。
//
// ============================ 当前能看到什么，看不到什么 ============================
//
// CPU / ASCII 后端会采样 baseColor 与 emissive 贴图，并支持 alpha mask；
// 法线 / AO / 粗糙度 / 金属度贴图暂不参与软件着色。
//
// 键位（与 viewer_ascii 相同）：
//     w / s 前进/后退   a / d 平移   q / e 降/升
//     h / l 转向        k / j 俯仰
//     f 线框   c 剔除   Esc 退出
//
// 用法：
//     build/viewer_asset                          # 默认模型
//     build/viewer_asset assets/model/mia/scene.gltf
//     MY3D_LOG_LEVEL=debug build/viewer_asset     # 看导入降级警告

#include "ascii_demo_common.hpp"
#include "asset/importer.hpp"
#include "backend/ascii/ascii_renderer.hpp"
#include "backend/cpu/scene_draw.hpp"
#include "logger.hpp"
#include "platform/tty/tty_window.hpp"
#include "render/animation_driver.hpp"
#include "render/camera_controller.hpp"
#include "render/scene_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using namespace my3d;
using namespace my3d::demo;

// 仓库根。由 CMake 注入（见 samples/CMakeLists.txt），是下面默认模型路径的解析
// 根。运行时 cwd 不一定是仓库根 —— 从 build/ 里启动、或 IDE 指定别的工作目录都
// 会变 —— 所以默认值不能是裸相对路径，否则 `cd build && ./viewer_asset` 会去
// build/assets/... 找一个不存在的文件。
//
// 只钉默认值：命令行显式给出的路径仍按 cwd 解析（那是用户自己的语义，不该被改）。
#ifndef MY3D_ASSET_ROOT
#define MY3D_ASSET_ROOT "."
#endif

// 默认模型：cube.fbx 只有 36 KB（看效果太单薄），alienCIty / buildings 是几十 MB
// 的整条街区（软件光栅在终端网格下会拖成个位数帧率）。car 是这三者之间的中档，
// 11 MB、纹理齐全、轮廓在字符网格上认得出。
constexpr const char *kDefaultModel = MY3D_ASSET_ROOT "/assets/model/car/FINAL_MODEL_B.fbx";

// 导入期把贴图压到这个边长以内。
//
// ASCII 后端会采样贴图，但字符画面不需要原始高分辨率：一张 4K 贴图就是 64 MB。
// 512 是画质与内存的折中，将来需要时可调大或传 0（不限制）。
constexpr uint32_t kMaxTextureSize = 512;

// 取景时选定的「身高」轴的符号。Sketchfab 导出的角色（node 里带 Rx(-90°)）把人体
// +Y（头方向）映到世界 -Z，所以头在 -Z 端，屏幕 up 得取 -Z 才不倒立。
// 按模型的世界包围盒自动取景。
//
// 这一步不能省：导入模型的尺度是未知量 —— glTF 角色可能是 1.7 米高，街区模型可能
// 是几百单位宽，assimp 又不做单位归一（它是文件里的什么数就是什么数）。写死一个
// 相机位置必然对某类资产错得离谱：要么相机埋在模型内部，要么模型小成一个点。
void frameCamera(scene::Camera &camera, const math::AABB &bounds)
{
    // 空包围盒（模型没有几何）时退回 viewer_ascii 的那套默认值，至少能看到
    // 一盏灯下的空场景，而不是一个 NaN 相机。
    if (!bounds.valid)
    {
        LOG_WARN("asset scene has no geometry bounds; falling back to a default camera");
        camera = scene::Camera::lookingAt({3.4f, 2.6f, 4.6f}, {0.0f, 0.6f, 0.0f});
        return;
    }

    const math::Vec3 center = bounds.center();
    const math::Vec3 extent = bounds.extent(); // 半长，不是全长
    const float radius = std::max(math::length(extent), 1e-4f);

    // 视锥的竖直半角决定「多远才装得下半径 r 的球」：d = r / sin(fovY/2)。
    // 1.15 是留白，免得模型正好顶到画面边缘。
    const float distance = radius / std::sin(camera.fovYRadians * 0.5f) * 1.15f;

    // 屏幕 up 恒取世界 +Y。
    //
    // 这里曾经按「包围盒最长轴」选 up，想救 Sketchfab 那类把 Rx(-90°) 写进节点的
    // 角色。那是错的：那条最长轴取自**绑定姿态**的包围盒，而动画轨道一旦驱动那批
    // 节点，写死的 Rx(-90°) 就被覆盖，模型在动画里其实是 +Y 竖直的。照绑定姿态
    // 取景会把相机绕到水平轴上，画面里的人横躺着且被透视压扁 —— 症状看着像蒙皮
    // 出错，其实是取景选错了轴。世界 +Y 是场景层与所有后端的共同约定，跟着走。
    const math::Vec3 up{0.0f, 1.0f, 0.0f};

    // 视线必须与 up 垂直，否则 up 与 forward 共线、相机基退化（见 Camera::right）。
    // 把四分之三视角投影掉 up 分量，得到一条水平视线。
    math::Vec3 direction = math::normalize(math::Vec3{0.55f, 0.42f, 0.72f});
    direction = direction - up * math::dot(direction, up);
    if (math::lengthSq(direction) < 1e-12f)
        direction = math::Vec3{0.0f, 0.0f, 1.0f};
    direction = math::normalize(direction);

    camera = scene::Camera::lookingAt(center + direction * distance, center, up);

    // near/far 跟着尺度走：写死 0.1 / 1000 在街区模型上会把整座城切进 near 平面。
    camera.nearPlane = std::max(radius * 0.01f, 1e-3f);
    camera.farPlane = distance + radius * 4.0f;
}

void printUsage(const char *program)
{
    std::fprintf(stderr,
                 "usage: %s [--animation clip-name] [model-path]\n"
                 "\n"
                 "  model-path   .fbx / .glb / .gltf 等 assimp 支持的格式\n"
                 "               省略时用 %s\n"
                 "               显式路径按当前工作目录解析\n"
                 "  --animation  按名称播放动画；省略时播放第一条\n"
                 "\n"
                 "  环境变量 MY3D_LOG_LEVEL = trace|debug|info|warn|error|fatal|off\n"
                 "\n"
                 "仓库里可用的模型：\n"
                 "  assets/model/car/FINAL_MODEL_B.fbx   11 MB，默认\n"
                 "  assets/model/mia/scene.gltf          28 MB，人像\n"
                 "  assets/model/cube/cube.fbx           36 KB，最小\n"
                 "  assets/model/alienCIty/Untitled.glb  46 MB，整条街区（很重）\n"
                 "  assets/model/buildings/All.fbx       68 MB，整条街区（很重）\n",
                 program, kDefaultModel);
}

} // namespace

int main(int argc, char **argv)
{
    using Clock = std::chrono::steady_clock;

    configureLogging();

    // ------------------------------------------------------------------
    // 命令行。只认一个模型路径与一个动画名称，别的都当错误 —— 猜用户想加载
    // 哪个文件/动画不如直接说清楚。
    // ------------------------------------------------------------------
    std::string modelPath = kDefaultModel;
    std::string animationName;
    int positional = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--animation")
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0')
            {
                std::fprintf(stderr, "viewer_asset: --animation needs a clip name\n");
                printUsage(argv[0]);
                return 2;
            }
            animationName = argv[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-')
        {
            std::fprintf(stderr, "viewer_asset: unknown option '%.*s'\n", (int)arg.size(), arg.data());
            printUsage(argv[0]);
            return 2;
        }
        if (++positional > 1)
        {
            std::fprintf(stderr, "viewer_asset: only one model path is accepted\n");
            printUsage(argv[0]);
            return 2;
        }
        modelPath = std::string{arg};
    }

    // ------------------------------------------------------------------
    // 导入。**在构造 TtyWindow 之前**：导入失败（文件不存在、assimp 解析不了）
    // 时没必要把终端切进交替屏幕走一趟，错误直接留在正常终端回滚缓冲里。
    // ------------------------------------------------------------------
    asset::ImportOptions importOptions;
    importOptions.maxTextureSize = kMaxTextureSize;

    LOG_INFO("importing '{}' ...", modelPath);

    const auto importStart = Clock::now();
    asset::ImportResult imported = asset::importScene(modelPath, importOptions);
    const double importSeconds = std::chrono::duration<double>(Clock::now() - importStart).count();

    if (!imported.ok)
    {
        LOG_ERROR("import failed: {}", imported.error);
        // 出错时 stdout 还没被接管，可以直接写 —— 用户看到的顺序才和实际一致。
        std::fprintf(stderr, "viewer_asset: cannot import '%s'\n", modelPath.c_str());
        return 1;
    }

    LOG_INFO("imported in {:.2f}s: {} nodes, {} meshes ({} skinned), {} materials, "
             "{} images, {} vertices, {} triangles, {} animations",
             importSeconds,
             imported.stats.nodeCount, imported.stats.meshCount,
             imported.stats.skinnedMeshCount, imported.stats.materialCount,
             imported.stats.textureCount, imported.stats.vertexCount,
             imported.stats.triangleCount, imported.stats.animationCount);

    // 导入器的降级记录一律转发出来。缺失贴图 / 不支持的属性会走到这里 ——
    // 它们是「画面为什么和预期不一样」的第一手线索，不能只在 debug 级别可见。
    for (const std::string &warning : imported.warnings)
        LOG_WARN("import: {}", warning);

    // ------------------------------------------------------------------
    // 渲染器先建：它同时是 IRenderDevice，装配阶段就要用它建资源。
    // 构造不碰终端（只准备内部缓冲），所以在 TtyWindow 之前是安全的。
    // ------------------------------------------------------------------
    backend::ascii::AsciiRenderer renderer;

    scene::World world;
    const render::SceneBuildResult built =
        render::buildWorldFromAsset(imported.scene, renderer, world);

    if (!built.ok)
    {
        LOG_ERROR("scene build failed: {}", built.error);
        std::fprintf(stderr, "viewer_asset: cannot build scene from '%s'\n", modelPath.c_str());
        return 1;
    }

    for (const std::string &warning : built.warnings)
        LOG_WARN("build: {}", warning);

    LOG_INFO("built: {} nodes, {} primitives, {} meshes, {} materials{}",
             built.stats.nodes, built.stats.primitives, built.stats.meshes, built.stats.materials,
             built.stats.skippedPrimitives ? " (+" + std::to_string(built.stats.skippedPrimitives) +
                                                 " skipped)" : "");

    // ------------------------------------------------------------------
    // 灯。**必须在 build 之后**：buildWorldFromAsset 是覆盖语义，它调用的
    // World::clear() 会把 lights 和 environment 一起清掉。反过来写不会报错，
    // 只会得到一个全黑（只剩环境项 0.02）的画面。
    // ------------------------------------------------------------------
    scene::LightDesc keyLight;
    keyLight.name = "key";
    keyLight.type = scene::LightType::Directional;
    keyLight.direction = {-0.45f, -1.0f, -0.35f};
    keyLight.color = {1.0f, 0.96f, 0.88f};
    keyLight.intensity = 3.2f;
    world.addLight(keyLight);

    scene::LightDesc fillLight;
    fillLight.name = "fill";
    fillLight.type = scene::LightType::Directional;
    fillLight.direction = {0.6f, -0.35f, 0.5f};
    fillLight.color = {0.35f, 0.42f, 0.6f};
    fillLight.intensity = 0.9f;
    world.addLight(fillLight);

    const math::AABB sceneBounds = world.worldBounds();
    if (sceneBounds.valid)
    {
        const math::Vec3 boundsCenter = sceneBounds.center();
        const math::Vec3 boundsExtent = sceneBounds.extent();
        LOG_INFO("world bounds center ({:.3f}, {:.3f}, {:.3f}), "
                 "half-extent ({:.3f}, {:.3f}, {:.3f})",
                 boundsCenter.x, boundsCenter.y, boundsCenter.z,
                 boundsExtent.x, boundsExtent.y, boundsExtent.z);
    }

    // ------------------------------------------------------------------
    // 骨骼动画。绑定失败不终止程序：静态姿势仍然是可看的，比起把整个模型
    // 丢掉，把「动画没接上」这条原因打出来更有用。
    // ------------------------------------------------------------------
    render::AnimationDriver driver;

    if (imported.scene.animations.empty())
    {
        if (!animationName.empty())
        {
            LOG_ERROR("requested animation '{}' but the asset has no animations", animationName);
            return 1;
        }
        LOG_INFO("no animation in this asset; showing the bind pose");
    }
    else
    {
        const bool bound = animationName.empty()
                               ? driver.bind(imported.scene, 0u)
                               : driver.bind(imported.scene, animationName);
        if (bound)
        {
            const render::AnimationDriver::Stats &anim = driver.stats();
            LOG_INFO("animation '{}' bound: {:.2f}s, {}/{} channels resolved, {} duplicate, "
                     "{} skinned mesh(es), {} vertices",
                     driver.clipName(), driver.duration(), anim.resolvedChannels,
                     anim.clipChannels, anim.duplicateChannels, anim.skinnedMeshes,
                     anim.skinnedVertices);

            if (anim.unresolvedChannels > 0)
                LOG_WARN("animation: {} of {} channel(s) matched no node name",
                         anim.unresolvedChannels, anim.clipChannels);

            // 蒙皮内核自检。它验的不是「动画演得对不对」（那要肉眼看画面），而是
            // 「骨骼停在绑定姿态时蒙皮是否原样返回」。偏差应当在浮点噪声量级
            // （1e-6 以下）；一旦到 1e-3，无论后面播什么都带着同一份变形。
            const float kernelError = driver.skinningKernelError();
            if (kernelError >= 0.0f)
                LOG_INFO("skinning self-check: max bind-pose deviation {:.3e} units",
                         static_cast<double>(kernelError));
        }
        else if (!animationName.empty())
        {
            LOG_ERROR("animation bind failed: {}", driver.error());
            return 1;
        }
        else
        {
            LOG_WARN("animation bind failed: {}", driver.error());
        }
    }

    // ------------------------------------------------------------------
    // 终端。到这里为止都还是普通终端；这一步之后 stdout 就归帧数据了。
    // ------------------------------------------------------------------
    platform::TtyWindow window;

    LOG_INFO("viewer_asset: terminal {}x{} cells", window.width(), window.height());

    renderer.resize(window.width(), window.height());

    // ------------------------------------------------------------------
    // 取景的包围盒要取自【动画姿势】，不能取自绑定姿态。
    //
    // 上面那条 sceneBounds 是绑定姿态的。对 Sketchfab 那类把 Rx(-90°) 写死在节点
    // 上的角色，动画轨道驱动的正是那批节点，播起来之后写死的旋转被覆盖，模型在
    // 世界里从「横躺」变成「+Y 竖直」。拿绑定姿态的包围盒去 frameCamera，中心与
    // 半长都落在错的轴上，画面里的人既偏出画面又横着躺。
    //
    // 所以先采样一帧动画（t = 0）把 World 推到动画姿势，再取一次包围盒。t = 0
    // 是安全的采样点：它要么是资产的自然站姿，要么就是动画的第一帧。
    // ------------------------------------------------------------------
    math::AABB viewBounds = sceneBounds;
    if (driver.bound())
    {
        if (driver.apply(0.0f, world, renderer, built.meshHandles))
        {
            // 注意：这里**不能**用 world.worldBounds()。它由 Node::meshBounds 合成，
            // 而那份数据是构建场景时从绑定姿态顶点算的，蒙皮结果从不回写 ——
            // 动画把模型从横躺推到竖直之后，worldBounds 仍描述绑定姿态那个扁盒子。
            //
            // 真正该用的是已经蒙好的顶点。meshBoundsDiag 读的正是驱动每帧上传的
            // 那份 vertices，逐网格给出节点局部空间的包围盒，乘节点世界矩阵就是
            // 渲染时模型真正占据的空间。
            const std::vector<math::Mat4> &nodeWorld = world.worldMatrices();
            math::AABB animated;
            for (const render::AnimationDriver::MeshBounds &mb : driver.meshBoundsDiag(world))
            {
                if (!mb.box.valid || mb.nodeIndex >= nodeWorld.size())
                    continue;
                animated.expand(mb.box.transformed(nodeWorld[mb.nodeIndex]));
            }

            if (animated.valid)
                viewBounds = animated;
            else
                LOG_WARN("the animated pose produced no usable bounds; framing on the bind pose");

            const math::Vec3 c = viewBounds.center();
            const math::Vec3 e = viewBounds.extent();
            LOG_INFO("framing bounds from the skinned pose: center ({:.3f}, {:.3f}, {:.3f}), "
                     "half-extent ({:.3f}, {:.3f}, {:.3f})",
                     c.x, c.y, c.z, e.x, e.y, e.z);
        }
        else
        {
            LOG_WARN("cannot sample the animation for framing: {}", driver.error());
        }
    }

    // ------------------------------------------------------------------
    // 相机与输入
    // ------------------------------------------------------------------
    render::CameraController controller;
    controller.camera() = scene::Camera{};
    frameCamera(controller.camera(), viewBounds);

    // 移动速度跟着模型尺度走：同一个 4.0 单位/秒在摩托车模型上是冲刺，
    // 在街区模型上是爬。半径的量级才是「一眼能看出在动」的量级。
    const float radius = viewBounds.valid ? math::length(viewBounds.extent()) : 1.0f;
    controller.setMoveSpeed(std::max(radius * 1.2f, 0.1f));
    controller.setLookSpeed(0.0025f);

    {
        const math::Vec3 p = controller.camera().position;
        LOG_INFO("camera at ({:.3f}, {:.3f}, {:.3f}), move speed {:.3f}/s, "
                 "near {:.4f}, far {:.1f}",
                 p.x, p.y, p.z, radius * 1.2f,
                 controller.camera().nearPlane, controller.camera().farPlane);
    }

    render::View view;
    view.settings.showEnvironment = false;   // 没有 IBL 资源，只靠两盏方向光
    // ASCII 后端按 sRGB 感知亮度选字形（见 backend/cpu/framebuffer.hpp 的
    // extractLuminance），所以「看起来很深」的线性色并不深：linearToSrgb 会把
    // (0.02, 0.02, 0.035) 抬到约 0.15 的 ink，正好卡在 ramp " .:-=+*#%@" 的
    // '=' / '+' 两档之间，Bayer 抖动就把整片背景切成规则网格。
    // 想背景真正留白（落到 coverage[0] = 空格），只有纯黑。
    view.settings.clearColor = {0.0f, 0.0f, 0.0f};
    view.settings.wireframe = false;

    render::ButtonState buttons;
    std::vector<render::InputEvent> events;

    constexpr double kTargetFrameSeconds = 1.0 / 60.0;

    auto previous = Clock::now();
    double timeSeconds = 0.0;
    uint64_t frames = 0;

    while (window.poll(buttons, events))
    {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        timeSeconds += dt;

        for (const render::InputEvent &event : events)
        {
            switch (event.type)
            {
            case render::InputEvent::Type::Resize:
                renderer.resize(event.width, event.height);
                break;
            case render::InputEvent::Type::ToggleWireframe:
                view.settings.wireframe = !view.settings.wireframe;
                break;
            case render::InputEvent::Type::ToggleCulling:
                view.settings.enableCulling = !view.settings.enableCulling;
                break;
            }
        }
        events.clear();

        controller.apply(toAction(buttons), dt);

        // 动画必须在 beginFrame 之前推进：它改的是 World 的节点变换和后端的
        // 顶点缓冲，这一帧读到的必须是推进后的状态。放在 beginFrame 之后会让
        // 姿势永远慢一帧 —— 单帧看不出来，但每一帧都错。
        if (driver.bound())
            driver.apply(timeSeconds, world, renderer, built.meshHandles);

        view.camera = controller.camera();
        const render::FrameInfo frameInfo{&world, &view, timeSeconds};

        if (!renderer.beginFrame(frameInfo))
        {
            const backend::ascii::AsciiStats &grid = renderer.stats();
            LOG_ERROR("beginFrame failed: grid is {}x{}", grid.gridWidth, grid.gridHeight);
            break;
        }

        renderer.endFrame();

        // 一帧的可观测性。
        //
        // 这几行不是调试残留：字符输出是**差值**的，所以「几何被全部剔掉」
        // 「相机朝向错了」「三角形数根本是 0」三种故障在终端上长得一模一样 ——
        // 都是「什么都没变，一个字节都不发」。只有统计量能把它们分开。
        if (frames < 3 || frames % 300 == 0)
        {
            const backend::cpu::SceneDrawStats &sd = renderer.sceneStats();
            const backend::cpu::RasterStats &rs = renderer.rasterStats();
            LOG_DEBUG("frame {}: items {}/{} culled, calls {}, tris {} -> drawn {}, "
                      "backface-culled {}, clip-dropped {}, frags tested/shaded {}/{}, "
                      "glyphs {}, bytes {}, {:.2f} ms",
                      frames, sd.drawItems, sd.drawItemsCulled, sd.drawCalls, sd.triangles,
                      rs.trianglesDrawn, rs.trianglesCulled, rs.trianglesClipped,
                      rs.fragmentsTested, rs.fragmentsShaded, renderer.stats().glyphs,
                      renderer.stats().bytes, renderer.lastFrameSeconds() * 1000.0);

            if (driver.bound())
            {
                const render::AnimationDriver::Stats &anim = driver.stats();
                LOG_DEBUG("animation: {} mesh(es) uploaded, {} failed, {} uncompensated, "
                          "{} invalid joint(s)",
                          anim.uploadedMeshes, anim.failedUploads, anim.uncompensatedMeshes,
                          anim.invalidJoints);

                // 每个网格的实际包围盒。首帧打一次就够 —— 它拿到的是这一段
                // 补偿链路（网格归属 → 关节矩阵 → 上传）的最终产物。
                if (frames < 3)
                {
                    for (const render::AnimationDriver::MeshBounds &bounds :
                         driver.meshBoundsDiag(world))
                    {
                        const auto bindCenter = bounds.bindBox.center();
                        const auto bindHalf = bounds.bindBox.extent();
                        const auto center = bounds.box.center();
                        const auto half = bounds.box.extent();
                        LOG_DEBUG("  mesh {} <- node {} ({} ref(s)): nodeScale ({:.4f}, {:.4f}, "
                                  "{:.4f}) | bind center ({:.1f}, {:.1f}, {:.1f}) half ({:.1f}, "
                                  "{:.1f}, {:.1f}) | skinned center ({:.1f}, {:.1f}, {:.1f}) "
                                  "half ({:.1f}, {:.1f}, {:.1f})",
                                  bounds.meshIndex, bounds.nodeIndex, bounds.ownerCount,
                                  bounds.nodeScale.x, bounds.nodeScale.y, bounds.nodeScale.z,
                                  bindCenter.x, bindCenter.y, bindCenter.z,
                                  bindHalf.x, bindHalf.y, bindHalf.z,
                                  center.x, center.y, center.z, half.x, half.y, half.z);
                    }
                }
            }
        }

        const std::string &frameBytes = renderer.output();
        if (!frameBytes.empty() && !writeAll(frameBytes))
            break;

        ++frames;

        const double spent = std::chrono::duration<double>(Clock::now() - now).count();
        if (spent < kTargetFrameSeconds)
            std::this_thread::sleep_for(std::chrono::duration<double>(kTargetFrameSeconds - spent));
    }

    const backend::ascii::AsciiStats &stats = renderer.stats();
    LOG_INFO("{} frames, grid {}x{}, {} glyphs, {} edge cells, "
             "last frame {:.2f} ms / {} bytes",
             frames, stats.gridWidth, stats.gridHeight, stats.glyphs, stats.edgeCells,
             renderer.lastFrameSeconds() * 1000.0, stats.bytes);
    return 0;
}
