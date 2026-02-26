#pragma once

#include "logger.hpp"
#include "engine.hpp"
#include "camera.hpp"
#include <chrono>
#include <bitset>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#undef Success
#include <filament/SwapChain.h>
#include <filament/Viewport.h>

class Window_SDL
{
    bool isRunning_ = false;
    SDL_Window *window_ = nullptr;
    filament::SwapChain *swapChain_ = nullptr;
    int32_t lastX_ = 0;
    int32_t lastY_ = 0;
    Camera cameraCPU_;
    std::chrono::time_point<std::chrono::steady_clock> lastTimepoint_;
    enum class STATUS
    {
        FRONT,
        BACK,
        LEFT,
        RIGHT,
        UP,
        DOWN,
        count
    };
    std::bitset<static_cast<size_t>(STATUS::count)> statusMapping_;

public:
    Window_SDL() noexcept = default;
    ~Window_SDL() noexcept = default;
    Window_SDL(const Window_SDL &) = delete;
    Window_SDL &operator=(const Window_SDL &) = delete;
    Window_SDL(Window_SDL &&) noexcept = delete;
    Window_SDL &operator=(Window_SDL &&) noexcept = delete;
    void initWindow(Engine &engine) noexcept
    {
        SDL_Init(SDL_INIT_VIDEO |
                 SDL_INIT_EVENTS);
        window_ = SDL_CreateWindow("my3d",
                                   SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED,
                                   1280,
                                   720,
                                   SDL_WINDOW_VULKAN |
                                       SDL_WINDOW_RESIZABLE);
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(window_, &wmInfo);
        void *nativeWindow = nullptr;
#if defined(SDL_VIDEO_DRIVER_X11)
        nativeWindow = (void *)wmInfo.info.x11.window;
#elif defined(SDL_VIDEO_DRIVER_WAYLAND)
        nativeWindow = wmInfo.info.wl.surface;
#else
#error "SDL macro needed"
#endif
        swapChain_ = engine.getEngine()->createSwapChain(nativeWindow);
        auto view = engine.getView("myView");
        view->setViewport({0, 0, 1280, 720});
        auto scene = engine.getScene("myScene");
        auto light = engine.getEntity("myLight");
        filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
            .color(filament::Color::toLinear<filament::ACCURATE>(filament::sRGBColor(0.98f, 0.92f, 0.89f)))
            .intensity(80000.0f)
            .direction({0.0f, -1.0f, -1.0f})
            .castShadows(true) //
            .build(*engine.getEngine(), light);
        scene->addEntity(light);
        view->setScene(scene);
        auto camera = engine.getCamera("myCamera");
        view->setCamera(camera);
        camera->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        camera->setProjection(cameraCPU_.fovY, cameraCPU_.aspect, cameraCPU_.nearLimit, cameraCPU_.farLimit, filament::Camera::Fov::VERTICAL);
        isRunning_ = true;
    }
    inline bool isRunning() noexcept
    {
        return isRunning_;
    }
    inline void updateWindow(Engine &engine) noexcept
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            check4test(event, engine);
        }
        auto now = std::chrono::steady_clock::now();
        updateStatus(engine, std::chrono::duration<double>(now - lastTimepoint_).count());
        lastTimepoint_ = now;

        auto renderer = engine.getRenderer();
        if (renderer->beginFrame(swapChain_))
        {
            // renderer->setClearOptions({.clearColor = {0.2f, 0.4f, 0.6f, 1.0f}, .clear = true});
            renderer->render(engine.getView("myView"));
            renderer->endFrame();
        }
    }

private:
    inline void updateStatus(Engine &engine, double deltaSeconds) noexcept
    {
        if (statusMapping_.test((size_t)STATUS::FRONT))
        {
            cameraCPU_.position += cameraCPU_.front * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
        if (statusMapping_.test((size_t)STATUS::BACK))
        {
            cameraCPU_.position -= cameraCPU_.front * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
        if (statusMapping_.test((size_t)STATUS::LEFT))
        {
            cameraCPU_.position += normalize(cross(filament::math::double3{0, 1, 0}, cameraCPU_.front)) * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
        if (statusMapping_.test((size_t)STATUS::RIGHT))
        {
            cameraCPU_.position += normalize(cross(cameraCPU_.front, filament::math::double3{0, 1, 0})) * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
        if (statusMapping_.test((size_t)STATUS::UP))
        {
            cameraCPU_.position += normalize(cross(cameraCPU_.front, cross(filament::math::double3{0, 1, 0}, cameraCPU_.front))) * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
        if (statusMapping_.test((size_t)STATUS::DOWN))
        {
            cameraCPU_.position += normalize(cross(cameraCPU_.front, cross(cameraCPU_.front, filament::math::double3{0, 1, 0}))) * deltaSeconds * cameraCPU_.moveFactor;
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
        }
    }
    inline void check4test(SDL_Event &event, Engine &engine) noexcept
    {
        switch (event.type)
        {
        case SDL_QUIT:
        {
            isRunning_ = false;
        }
        break;
        case SDL_WINDOWEVENT:
        {
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                uint32_t w = event.window.data1;
                uint32_t h = event.window.data2;
                engine.getView("myView")->setViewport({0, 0, w, h});
                LOG_INFO("resize: {} x {}", w, h);
            }
        }
        break;
        case SDL_MOUSEMOTION:
        {
            auto x = event.motion.x;
            auto y = event.motion.y;
            cameraCPU_.yawRad += (x - lastX_) * cameraCPU_.yawFactor;
            constexpr double DOUBLE_PI = 2.0 * std::numbers::pi;
            cameraCPU_.yawRad = std::remainder(cameraCPU_.yawRad, DOUBLE_PI);
            cameraCPU_.pitchRad += (lastY_ - y) * cameraCPU_.pitchFactor;
            constexpr double HALF_PI = 0.5 * std::numbers::pi;
            cameraCPU_.pitchRad = std::clamp(cameraCPU_.pitchRad, -HALF_PI, HALF_PI);
            cameraCPU_.front.x = std::cos(cameraCPU_.pitchRad) * std::cos(cameraCPU_.yawRad);
            cameraCPU_.front.y = std::sin(cameraCPU_.pitchRad);
            cameraCPU_.front.z = std::cos(cameraCPU_.pitchRad) * std::sin(cameraCPU_.yawRad);
            cameraCPU_.front = normalize(cameraCPU_.front);
            engine.getCamera("myCamera")->lookAt(cameraCPU_.position, cameraCPU_.position + cameraCPU_.front);
            lastX_ = x;
            lastY_ = y;
            // LOG_INFO("cursor: {}, {}", x, y);
        }
        break;
        case SDL_MOUSEBUTTONDOWN:
        {
            LOG_INFO("mouse: button={}, state={}", event.button.button, event.button.state);
        }
        break;
        case SDL_MOUSEBUTTONUP:
        {
            LOG_INFO("mouse: button={}, state={}", event.button.button, event.button.state);
        }
        break;
        case SDL_MOUSEWHEEL:
        {
            cameraCPU_.fovY -= event.wheel.y * cameraCPU_.zoomFactor;
            cameraCPU_.fovY = std::clamp(cameraCPU_.fovY, 10.0, 120.0);
            engine.getCamera("myCamera")->setProjection(cameraCPU_.fovY, cameraCPU_.aspect, cameraCPU_.nearLimit, cameraCPU_.farLimit, filament::Camera::Fov::VERTICAL);
            LOG_INFO("scroll: {}, {}", event.wheel.x, event.wheel.y);
        }
        break;
        case SDL_KEYDOWN:
        {
            switch (event.key.keysym.scancode)
            {
            case SDL_SCANCODE_ESCAPE:
            {
            }
            break;
            case SDL_SCANCODE_W:
            {
                statusMapping_.set((size_t)STATUS::FRONT);
            }
            break;
            case SDL_SCANCODE_A:
            {
                statusMapping_.set((size_t)STATUS::LEFT);
            }
            break;
            case SDL_SCANCODE_S:
            {
                statusMapping_.set((size_t)STATUS::BACK);
            }
            break;
            case SDL_SCANCODE_D:
            {
                statusMapping_.set((size_t)STATUS::RIGHT);
            }
            break;
            case SDL_SCANCODE_SPACE:
            {
                statusMapping_.set((size_t)STATUS::UP);
            }
            break;
            case SDL_SCANCODE_LCTRL:
            {
                statusMapping_.set((size_t)STATUS::DOWN);
            }
            break;
            default:
                break;
            }
            LOG_INFO("key: {}, state={}", SDL_GetScancodeName(event.key.keysym.scancode), static_cast<int>(event.key.state));
        }
        break;
        case SDL_KEYUP:
        {
            switch (event.key.keysym.scancode)
            {
            case SDL_SCANCODE_ESCAPE:
            {
                isRunning_ = false;
            }
            break;
            case SDL_SCANCODE_W:
            {
                statusMapping_.reset((size_t)STATUS::FRONT);
            }
            break;
            case SDL_SCANCODE_A:
            {
                statusMapping_.reset((size_t)STATUS::LEFT);
            }
            break;
            case SDL_SCANCODE_S:
            {
                statusMapping_.reset((size_t)STATUS::BACK);
            }
            break;
            case SDL_SCANCODE_D:
            {
                statusMapping_.reset((size_t)STATUS::RIGHT);
            }
            break;
            case SDL_SCANCODE_SPACE:
            {
                statusMapping_.reset((size_t)STATUS::UP);
            }
            break;
            case SDL_SCANCODE_LCTRL:
            {
                statusMapping_.reset((size_t)STATUS::DOWN);
            }
            break;
            default:
                break;
            }
            LOG_INFO("key: {}, state={}", SDL_GetScancodeName(event.key.keysym.scancode), static_cast<int>(event.key.state));
        }
        break;
        case SDL_TEXTINPUT:
        {
            LOG_INFO("char: {}", event.text.text);
        }
        break;
        default:
            break;
        }
    }
};
