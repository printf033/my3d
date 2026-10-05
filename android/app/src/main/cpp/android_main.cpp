#include "asset/importer.hpp"
#include "backend/filament/filament_renderer.hpp"
#include "render/animation_driver.hpp"
#include "render/camera_controller.hpp"
#include "render/scene_builder.hpp"

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr char kLogTag[] = "my3d-android";
constexpr float kLookSpeed = 0.0025f;

void logInfo(const std::string &message)
{
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", message.c_str());
}

void logError(const std::string &message)
{
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message.c_str());
}

bool copyAsset(AAssetManager *manager, const std::string &assetPath,
               const std::filesystem::path &destination)
{
    AAsset *asset = AAssetManager_open(manager, assetPath.c_str(), AASSET_MODE_STREAMING);
    if (!asset)
    {
        logError("APK asset missing: " + assetPath);
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
    {
        AAsset_close(asset);
        logError("cannot create asset directory: " + error.message());
        return false;
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        AAsset_close(asset);
        logError("cannot write copied asset: " + destination.string());
        return false;
    }

    char buffer[32 * 1024];
    int bytesRead = 0;
    while ((bytesRead = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
        output.write(buffer, bytesRead);

    AAsset_close(asset);
    output.flush();
    if (bytesRead < 0 || !output)
    {
        logError("failed while copying APK asset: " + assetPath);
        return false;
    }
    return true;
}

bool prepareAssets(android_app *app, std::filesystem::path &assetRoot)
{
    assetRoot = std::filesystem::path(app->activity->internalDataPath) / "my3d";
    const std::vector<std::string> files = {
        "assets/model/mia/scene.gltf",
        "assets/model/mia/scene.bin",
        "assets/model/mia/license.txt",
        "assets/model/mia/textures/Mia_Body_baseColor.png",
        "assets/model/mia/textures/Mia_Face_baseColor.png",
        "assets/model/mia/textures/Mia_Face_Screen_baseColor.png",
        "assets/model/mia/textures/Mia_Face_Screen_normal.png",
        "assets/model/mia/textures/Mia_Hair_baseColor.png",
        "assets/model/mia/textures/Mia_Suit_baseColor.png",
        "assets/model/mia/textures/Mia_TerSuit_baseColor.png",
        "assets/shader/android/vulkan/unlit.filamat",
        "assets/shader/android/vulkan/lit.filamat",
        "assets/shader/android/vulkan/lit_aorm.filamat",
    };

    for (const std::string &file : files)
    {
        if (!copyAsset(app->activity->assetManager, file, assetRoot / file))
            return false;
    }
    return true;
}

struct AppState
{
    my3d::backend::filament::FilamentRenderer *renderer = nullptr;
    my3d::render::CameraController *camera = nullptr;
    bool hasWindow = false;
    bool focused = false;
    bool swapChainReady = false;
    bool pointerDown = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    float pointerDeltaX = 0.0f;
    float pointerDeltaY = 0.0f;
};

void onAppCommand(android_app *app, int32_t command)
{
    auto *state = static_cast<AppState *>(app->userData);
    if (!state)
        return;

    switch (command)
    {
    case APP_CMD_INIT_WINDOW:
        state->hasWindow = app->window != nullptr;
        state->swapChainReady = false;
        logInfo("Android window initialized.");
        break;
    case APP_CMD_TERM_WINDOW:
        state->hasWindow = false;
        state->swapChainReady = false;
        if (state->renderer)
            state->renderer->detachSwapChain();
        logInfo("Android window terminated.");
        break;
    case APP_CMD_GAINED_FOCUS:
        state->focused = true;
        logInfo("Android activity gained focus.");
        break;
    case APP_CMD_LOST_FOCUS:
        state->focused = false;
        state->pointerDown = false;
        logInfo("Android activity lost focus.");
        break;
    default:
        break;
    }
}

int32_t onInput(android_app *app, AInputEvent *event)
{
    auto *state = static_cast<AppState *>(app->userData);
    if (!state || !state->camera || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return 0;

    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const float x = AMotionEvent_getX(event, 0);
    const float y = AMotionEvent_getY(event, 0);
    if (action == AMOTION_EVENT_ACTION_DOWN)
    {
        state->pointerDown = true;
        state->pointerX = x;
        state->pointerY = y;
        return 1;
    }
    if (action == AMOTION_EVENT_ACTION_MOVE && state->pointerDown)
    {
        state->pointerDeltaX += x - state->pointerX;
        state->pointerDeltaY += y - state->pointerY;
        state->pointerX = x;
        state->pointerY = y;
        return 1;
    }
    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
    {
        state->pointerDown = false;
        return 1;
    }
    return 0;
}

void frameCamera(my3d::scene::Camera &camera, const my3d::math::AABB &bounds)
{
    if (!bounds.valid)
    {
        camera = my3d::scene::Camera::lookingAt({3.4f, 2.6f, 4.6f}, {0.0f, 0.6f, 0.0f});
        return;
    }

    const my3d::math::Vec3 center = bounds.center();
    const float radius = std::max(my3d::math::length(bounds.extent()), 1e-4f);
    const float distance = radius / std::sin(camera.fovYRadians * 0.5f) * 1.15f;
    const my3d::math::Vec3 direction =
        my3d::math::normalize(my3d::math::Vec3{0.55f, 0.42f, 0.72f});
    camera = my3d::scene::Camera::lookingAt(center + direction * distance, center);
    camera.nearPlane = std::max(radius * 0.01f, 1e-3f);
    camera.farPlane = distance + radius * 4.0f;
}

my3d::math::AABB animatedBounds(const my3d::render::AnimationDriver &animation,
                               const my3d::scene::World &world)
{
    my3d::math::AABB bounds = world.worldBounds();
    if (!animation.bound())
        return bounds;

    my3d::math::AABB skinnedBounds;
    const auto &nodeWorld = world.worldMatrices();
    for (const auto &mesh : animation.meshBoundsDiag(world))
    {
        if (mesh.box.valid && mesh.nodeIndex < nodeWorld.size())
            skinnedBounds.expand(mesh.box.transformed(nodeWorld[mesh.nodeIndex]));
    }
    return skinnedBounds.valid ? skinnedBounds : bounds;
}

void runAndroidViewer(android_app *app)
{
    app_dummy();
    AppState state;
    app->userData = &state;
    app->onAppCmd = onAppCommand;
    app->onInputEvent = onInput;

    try
    {
        std::filesystem::path assetRoot;
        if (!prepareAssets(app, assetRoot))
        {
            ANativeActivity_finish(app->activity);
            return;
        }

        const std::string modelPath = (assetRoot / "assets/model/mia/scene.gltf").string();
        my3d::asset::ImportOptions importOptions;
        importOptions.maxTextureSize = 512;
        my3d::asset::ImportResult imported = my3d::asset::importScene(modelPath, importOptions);
        if (!imported.ok)
        {
            logError("Mia import failed: " + imported.error);
            ANativeActivity_finish(app->activity);
            return;
        }
        logInfo("Mia imported: " + std::to_string(imported.stats.nodeCount) + " nodes, " +
                std::to_string(imported.stats.skinnedMeshCount) + " skinned meshes, " +
                std::to_string(imported.stats.animationCount) + " animations");

        my3d::backend::filament::MaterialPackPaths packs;
        packs.unlit = "assets/shader/android/vulkan/unlit.filamat";
        packs.lit = "assets/shader/android/vulkan/lit.filamat";
        packs.litAorm = "assets/shader/android/vulkan/lit_aorm.filamat";
        auto renderer = std::make_unique<my3d::backend::filament::FilamentRenderer>(
            assetRoot.string(), std::move(packs), ::filament::Engine::Backend::VULKAN);
        state.renderer = renderer.get();

        my3d::scene::World world;
        const my3d::render::SceneBuildResult built =
            my3d::render::buildWorldFromAsset(imported.scene, *renderer, world);
        if (!built.ok)
        {
            logError("scene build failed: " + built.error);
            ANativeActivity_finish(app->activity);
            return;
        }

        my3d::render::AnimationDriver animation;
        if (!animation.bind(imported.scene, "mixamo.com"))
        {
            logError("animation bind failed: " + animation.error());
            ANativeActivity_finish(app->activity);
            return;
        }
        if (!animation.apply(0.0f, world, *renderer, built.meshHandles))
        {
            logError("initial animation update failed");
            ANativeActivity_finish(app->activity);
            return;
        }
        logInfo("animation mixamo.com bound: " + std::to_string(animation.stats().resolvedChannels) +
                "/" + std::to_string(animation.stats().clipChannels) + " channels");

        my3d::scene::LightDesc key;
        key.name = "key";
        key.type = my3d::scene::LightType::Directional;
        key.direction = {-0.45f, -1.0f, -0.35f};
        key.color = {1.0f, 0.96f, 0.88f};
        key.intensity = 0.000032f;
        world.addLight(key);

        my3d::render::CameraController camera;
        frameCamera(camera.camera(), animatedBounds(animation, world));
        camera.setMoveSpeed(1.0f);
        camera.setLookSpeed(kLookSpeed);
        state.camera = &camera;

        my3d::render::View view;
        view.camera = camera.camera();
        view.settings.enableCulling = true;
        view.settings.enableDepthTest = true;
        view.settings.clearColor = {0.035f, 0.045f, 0.065f};

        logInfo("Ready: Vulkan Filament viewer on Android; drag to orbit, Mia animation loops.");
        auto previous = std::chrono::steady_clock::now();
        auto start = previous;

        while (!app->destroyRequested)
        {
            int events = 0;
            android_poll_source *source = nullptr;
            while (ALooper_pollOnce(state.hasWindow && state.focused ? 0 : -1,
                                    nullptr, &events,
                                    reinterpret_cast<void **>(&source)) >= 0)
            {
                if (source)
                    source->process(app, source);
                if (app->destroyRequested)
                    break;
            }
            if (app->destroyRequested)
                break;
            if (!state.hasWindow || !state.focused || !app->window)
                continue;

            if (!state.swapChainReady)
            {
                logInfo("Creating Filament swapchain.");
                if (!renderer->attachSwapChain(app->window))
                {
                    logError("Filament could not attach the Android window.");
                    break;
                }
                state.swapChainReady = true;
                logInfo("Attached swapchain to Android window.");
            }

            const uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(app->window));
            const uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(app->window));
            if (width == 0 || height == 0)
                continue;
            renderer->resize(width, height);
            view.viewport = {width, height};
            view.camera.aspect = static_cast<float>(width) / static_cast<float>(height);

            const auto now = std::chrono::steady_clock::now();
            const double delta = std::chrono::duration<double>(now - previous).count();
            previous = now;
            const float elapsed = std::chrono::duration<float>(now - start).count();
            my3d::render::CameraAction action;
            action.yawDelta = state.pointerDeltaX * kLookSpeed;
            action.pitchDelta = -state.pointerDeltaY * kLookSpeed;
            state.pointerDeltaX = 0.0f;
            state.pointerDeltaY = 0.0f;
            camera.apply(action, delta);
            view.camera = camera.camera();

            if (!animation.apply(elapsed, world, *renderer, built.meshHandles))
            {
                logError("animation vertex upload failed");
                break;
            }

            const my3d::render::FrameInfo frame{&world, &view, elapsed};
            if (!renderer->beginFrame(frame))
            {
                logError("Filament beginFrame failed.");
                break;
            }
            renderer->endFrame();
            static bool firstFrame = true;
            if (firstFrame)
            {
                firstFrame = false;
                logInfo("First frame submitted at " + std::to_string(width) + "x" +
                        std::to_string(height) + ", primitives=" +
                        std::to_string(renderer->lastFramePrimitives()));
            }
        }

        state.camera = nullptr;
        state.renderer = nullptr;
    }
    catch (const std::exception &error)
    {
        logError(std::string("fatal viewer error: ") + error.what());
        ANativeActivity_finish(app->activity);
    }
}

} // namespace

void android_main(android_app *app)
{
    runAndroidViewer(app);
}
