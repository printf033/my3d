// samples/viewer_ascii.cpp
//
// 新架构的第一个端到端示例：把场景画成字符，直接跑在终端里。
//
// 这个可执行文件链接的库清单，就是「API 停留在引擎概念层」这句话的构建期证据：
//
//     viewer_ascii → my3d_backend_ascii → my3d_backend_cpu → my3d_core
//                  → my3d_platform_tty  → my3d_core
//
// 没有 Filament、没有 SDL2、没有 Vulkan、没有 assimp。这里写的场景描述代码
// （scene::World + scene::Camera + asset::MeshData）将来换成 Filament 后端时
// 一行都不改 —— 那是这套 API 存在的全部理由（见 docs/architecture.md §0）。
//
// 键位（物理键 → 语义按键在 platform/tty/tty_input.cpp，
//       语义按键 → 相机数学在 render/camera_controller.cpp）：
//
//     w / s          前进 / 后退
//     a / d          左 / 右平移
//     q / e          下降 / 上升
//     h / l          左转 / 右转
//     k / j          抬头 / 低头
//     ESC            退出（终端状态会恢复）

#include "ascii_demo_common.hpp"
#include "backend/ascii/ascii_renderer.hpp"
#include "logger.hpp"
#include "platform/tty/tty_window.hpp"
#include "render/camera_controller.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using namespace my3d;

// 日志接线 / 整帧写出 / 按键→相机动作 都在 ascii_demo_common.hpp 里，
// 与 viewer_asset 共用一份 —— 上游 mylog 演进时只需要改那个头。
using namespace my3d::demo;

// ---------------------------------------------------------------------------
// 示例几何
// ---------------------------------------------------------------------------

// 立方体，六个面各自独立顶点。
//
// 顶点不跨面共享是有意的：法线、切线、UV 在棱角处本来就不连续，硬要共享就得靠
// 平滑组或顶点分裂补救。那是导入器的职责，示例代码不该示范坏习惯。
//
// tangent.w 固定 +1：这里每个面都满足 cross(u, v) == normal，
// 副切线 b = cross(n, t) * w 正好落在面的 v 轴上。
asset::MeshData makeCube(float half)
{
    const math::Vec3 normals[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    const math::Vec3 axisU[6] = {
        {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0},
    };
    const math::Vec3 axisV[6] = {
        {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0},
    };
    const float cornerU[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float cornerV[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    asset::MeshData mesh;
    mesh.name = "cube";
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);

    for (int face = 0; face < 6; ++face)
    {
        const math::Vec3 n = normals[face];
        const math::Vec3 u = axisU[face];
        const math::Vec3 v = axisV[face];

        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        for (int corner = 0; corner < 4; ++corner)
        {
            asset::Vertex vertex;
            vertex.position = (n + u * cornerU[corner] + v * cornerV[corner]) * half;
            vertex.normal = n;
            vertex.tangent = {u.x, u.y, u.z, 1.0f};
            vertex.uv0 = {(cornerU[corner] + 1.0f) * 0.5f, (cornerV[corner] + 1.0f) * 0.5f};

            // bounds 由 MeshData 自带 —— 后端创建 mesh 时可以不看，但节点剔除要看，
            // 所以资源方（这里）算好、由 attachPrimitive 的调用方转写进 node。
            mesh.bounds.expand(vertex.position);
            mesh.vertices.push_back(vertex);
        }

        mesh.indices.push_back(base);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    }
    return mesh;
}

// 水平地面（y = 0），法线 +Y。
asset::MeshData makeGround(float halfSize)
{
    const math::Vec3 n{0, 1, 0};
    const math::Vec3 u{1, 0, 0};
    const math::Vec3 v{0, 0, -1};   // cross(u, v) == n
    const float cornerU[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float cornerV[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    asset::MeshData mesh;
    mesh.name = "ground";
    mesh.vertices.reserve(4);
    mesh.indices.reserve(6);

    for (int corner = 0; corner < 4; ++corner)
    {
        asset::Vertex vertex;
        vertex.position = u * (cornerU[corner] * halfSize) + v * (cornerV[corner] * halfSize);
        vertex.normal = n;
        vertex.tangent = {u.x, u.y, u.z, 1.0f};
        vertex.uv0 = {(cornerU[corner] + 1.0f) * 0.5f, (cornerV[corner] + 1.0f) * 0.5f};
        mesh.bounds.expand(vertex.position);
        mesh.vertices.push_back(vertex);
    }

    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

} // namespace

int main()
{
    using Clock = std::chrono::steady_clock;

    // 必须在任何输出之前：TtyWindow 一旦构造就进入交替屏幕，此后 stdout 不再是
    // 一个可以随便写的地方。
    configureLogging();

    backend::ascii::AsciiRenderer renderer;

    // 失败即抛 std::runtime_error（stdin/stdout 不是 tty 等），且抛出前终端状态
    // 已恢复 —— 所以这里可以直接让异常穿出去，不必自己兜底。
    platform::TtyWindow window;

    LOG_INFO("viewer_ascii: terminal {}x{} cells", window.width(), window.height());

    // AsciiRenderer::resize 的单位是字符列数 / 行数，不是像素。
    renderer.resize(window.width(), window.height());

    // ------------------------------------------------------------------
    // 场景：两个网格、两个材质、一盏方向光
    // ------------------------------------------------------------------
    scene::World world;

    const asset::MeshData cube = makeCube(0.6f);
    const asset::MeshData ground = makeGround(6.0f);

    const MeshHandle cubeMesh = renderer.createMesh(cube);
    const MeshHandle groundMesh = renderer.createMesh(ground);

    asset::MaterialDesc cubeMaterialDesc;
    cubeMaterialDesc.name = "cube";
    cubeMaterialDesc.model = asset::ShadingModel::Lit;
    cubeMaterialDesc.baseColorFactor = {0.85f, 0.58f, 0.24f, 1.0f};
    cubeMaterialDesc.roughnessFactor = 0.45f;
    cubeMaterialDesc.metallicFactor = 0.0f;

    asset::MaterialDesc groundMaterialDesc;
    groundMaterialDesc.name = "ground";
    groundMaterialDesc.model = asset::ShadingModel::Lit;
    // 基色按 ASCII 输出的动态范围标定，不是物理反射率。
    // 地面是水平面 + 方向光，ndotl 整屏为常量，于是 ink 也整屏为常量 ——
    // 这既是「地面必然是一整块同色」的原因，也是标定它的手段：只要让 ink
    // 离两档的边界足够远，整块地面就干净利落，不会在边界附近翻档。
    // 0.06 对应的 ink ≈ 0.29，落在 '*'（覆盖率 0.25）到 '#'（0.40）之间的
    // 偏 '*' 一侧，离 0.325 那条边界有富余；立方体的朝阳面因此能高出两三个
    // 档位，明暗层次才拉得开。想更暗就继续降，但要避开 0.325 附近。
    groundMaterialDesc.baseColorFactor = {0.06f, 0.0685f, 0.078f, 1.0f};
    groundMaterialDesc.roughnessFactor = 0.9f;

    const MaterialHandle cubeMaterial = renderer.createMaterial(cubeMaterialDesc);
    const MaterialHandle groundMaterial = renderer.createMaterial(groundMaterialDesc);

    const uint32_t cubeNode = world.createNode("cube");
    world.attachPrimitive(cubeNode, scene::Primitive{cubeMesh, cubeMaterial});
    world.node(cubeNode)->meshBounds = cube.bounds;   // 见下方注释
    world.node(cubeNode)->transform.position = {0.0f, 0.6f, 0.0f};

    const uint32_t groundNode = world.createNode("ground");
    world.attachPrimitive(groundNode, scene::Primitive{groundMesh, groundMaterial});
    world.node(groundNode)->meshBounds = ground.bounds;

    // 注意上面两行 meshBounds：attachPrimitive **不**替调用方填包围盒 ——
    // 它只持有「网格 + 材质」，而同一个网格被不同节点缩放时包围盒并不相同，
    // 所以这是调用方的契约。漏填的后果不是报错，而是视锥剔除对这两个节点
    // 永久失效（保守保留），画面正常、性能静默下降。见 docs/architecture.md §5.4
    // 的 `worldBounds`。

    scene::LightDesc keyLight;
    keyLight.name = "key";
    keyLight.type = scene::LightType::Directional;
    keyLight.direction = {-0.45f, -1.0f, -0.35f};
    keyLight.color = {1.0f, 0.96f, 0.88f};
    // 强度按 ASCII 输出的动态范围标定，不是物理照度：字符 ramp 只有 10 档，
    // 而直接光路径的能量是 albedo * ndotl * intensity（不含 1/π，见 render/toolbox.cpp
    // evalDiffuse 与 evalIrradianceSH 的对比）。地面是水平面、ndotl ≈ 0.87，
    // 3.2 会让它线性值冲到 ~0.6、tonemap 后仍高过 ramp 的顶端档，整片压成 '@'；
    // 1.0 把地面落在中灰档，亮部留给立方体的朝阳面，明暗层次才出得来。
    keyLight.intensity = 1.0f;
    world.addLight(keyLight);

    scene::LightDesc fillLight;
    fillLight.name = "fill";
    fillLight.type = scene::LightType::Directional;
    fillLight.direction = {0.6f, -0.35f, 0.5f};
    fillLight.color = {0.35f, 0.42f, 0.6f};
    fillLight.intensity = 0.35f;   // 与 key 同比例下调，保持冷暖比不变
    world.addLight(fillLight);

    // ------------------------------------------------------------------
    // 相机与输入
    // ------------------------------------------------------------------
    render::CameraController controller;
    controller.setMoveSpeed(4.0f);
    controller.setLookSpeed(0.0025f);
    controller.camera() = scene::Camera::lookingAt({3.4f, 2.6f, 4.6f}, {0.0f, 0.6f, 0.0f});

    render::View view;
    view.settings.showEnvironment = false;   // 没有 IBL 资源，只靠两盏方向光
    // ASCII 后端按 sRGB 感知亮度选字形（见 backend/cpu/framebuffer.hpp 的
    // extractLuminance）：linearToSrgb 会把 (0.02, 0.02, 0.035) 抬到约 0.15 的
    // ink，正好卡在 ramp " .:-=+*#%@" 的 '=' / '+' 两档之间，Bayer 抖动于是把
    // 整片背景切成规则网格。背景要留白（落到 coverage[0] = 空格）只能纯黑。
    view.settings.clearColor = {0.0f, 0.0f, 0.0f};

    // 关掉有序抖动。抖动是为「渐变」服务的：它在覆盖率空间里把 ink 落在相邻
    // 两档之间，再用 4x4 Bayer 阈值决定每个格点取上取还是取下，从而拿 10 档
    // 字符表现更多层次。代价是**均匀面**必然显出固定图案 —— 阈值是空间函数，
    // 常数 frac 就切出常数图案（这片水平地面在方向光下 ndotl 整屏常量，正是
    // 最坏情况：实测 800 格里 100 个 '*'、700 个 '#'，呈现出 4 格周期的格纹）。
    // 这里要的是「一块平的地」，不是「一层规则的格纹」，所以关掉；
    // 立方体的明暗过渡仍由字符档位本身表达，不依赖抖动。
    renderer.settings().glyph.dither = false;

    render::ButtonState buttons;
    std::vector<render::InputEvent> events;

    // 终端没有垂直同步。不节流的话主循环会尽可能快地跑 —— 实测 90x30 网格下
    // 约 2100 fps、单核 100%（3055 帧 × 0.47 ms ≈ 1.44 s 墙钟，正好等于观测时长）。
    // 键盘漫游 60 fps 绰绰有余，多出来的帧只是在烧电。
    constexpr double kTargetFrameSeconds = 1.0 / 60.0;

    auto previous = Clock::now();
    double timeSeconds = 0.0;
    uint64_t frames = 0;

    // poll 返回 false = 收到 Quit 或终端真的关了。
    while (window.poll(buttons, events))
    {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        timeSeconds += dt;

        // events 是「追加」语义，清空由调用方负责（buttons 相反，是完全覆盖）。
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

        view.camera = controller.camera();
        const render::FrameInfo frameInfo{&world, &view, timeSeconds};

        if (!renderer.beginFrame(frameInfo))
        {
            // 网格尺寸为 0：明确失败，不猜默认值。
            const backend::ascii::AsciiStats &grid = renderer.stats();
            LOG_ERROR("beginFrame failed: grid is {}x{}", grid.gridWidth, grid.gridHeight);
            break;
        }

        renderer.endFrame();

        const std::string &frameBytes = renderer.output();
        if (!frameBytes.empty() && !writeAll(frameBytes))
            break;

        ++frames;

        const double spent = std::chrono::duration<double>(Clock::now() - now).count();
        if (spent < kTargetFrameSeconds)
            std::this_thread::sleep_for(std::chrono::duration<double>(kTargetFrameSeconds - spent));
    }

    // 退出后的统计走 stderr：stdout 正被终端交替屏幕缓冲接管，此时写进去的内容
    // 会随退出屏幕一起消失。stderr 不参与交替缓冲，稳定可见 —— configureLogging
    // 已把 mylog 默认的 stdout sink 换成 stderr，这里只是让它生效。
    const backend::ascii::AsciiStats &stats = renderer.stats();
    LOG_INFO("{} frames, grid {}x{}, {} glyphs, {} edge cells, "
             "last frame {:.2f} ms / {} bytes",
             frames, stats.gridWidth, stats.gridHeight, stats.glyphs, stats.edgeCells,
             renderer.lastFrameSeconds() * 1000.0, stats.bytes);
    return 0;
}
