#pragma once

// my3d::scene —— 相机纯数据。
//
// 这里只有数学：位置 / 朝向 / fov / near / far -> view / proj / frustum。
// 不含任何后端类型，也不含输入映射。按键到动作的翻译见
// render/camera_controller.hpp，SDL 与 TTY 共用同一套手感。

#include "core/math.hpp"

#include <cmath>

namespace my3d::scene
{

// 朝向约定与 math::rotation(+Y, yaw) 保持一致，便于把相机当成普通节点使用：
//
//   yaw = 0, pitch = 0   =>  forward = (0, 0, -1)
//   forward = Ry(yaw) * Rx(pitch) * (0, 0, -1)
//           = (-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch))
//
// 后果：yaw 增大时视线绕 +Y 按右手定则旋转（俯视图上是逆时针）。
// 若希望「鼠标右移 = 视线右转」，符号由输入层吃掉（见 CameraController 注释）。
struct Camera
{
    math::Vec3 position{0.0f, 0.0f, 3.0f};
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float fovYRadians = 0.78539816339f;   // 45°
    float aspect = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;

    // pi/2 减约 1°，避免 forward 与 worldUp 共线导致 lookAt 退化。
    static constexpr float kPitchLimit = 1.55334303427f;

    static constexpr math::Vec3 worldUp() noexcept { return {0.0f, 1.0f, 0.0f}; }

    // 屏幕「朝上」的世界方向，默认 +Y。
    //
    // 这里与 yaw/pitch 的分工是刻意拆开的：yaw/pitch 决定**看向哪**，并且始终
    // 按「+Y 为上」的球坐标解释 —— 那是 CameraController 的手感契约；up 只决定
    // **画面绕视线的滚转**。两者解耦之后，换掉 up 不会让 WASD 的手感跟着变。
    //
    // 需要换 up 的场合是「资产在世界里不是竖着摆放」：Sketchfab 导出的角色会把
    // Rx(-90°) 写进节点，身高方向落在世界 Z 上，屏幕竖直轴继续用 +Y 的话人就
    // 斜躺在画面里。viewer_asset 的 frameCamera 因而按包围盒挑 up。
    math::Vec3 up{0.0f, 1.0f, 0.0f};

    math::Vec3 forward() const noexcept
    {
        const float cp = std::cos(pitchRadians);
        return {-std::sin(yawRadians) * cp,
                std::sin(pitchRadians),
                -std::cos(yawRadians) * cp};
    }

    math::Vec3 right() const noexcept
    {
        const math::Vec3 f = forward();
        // up 与 forward 共线时叉积是零向量，normalize 返回 {0,0,0}，整个相机基
        // 会静默塌掉（表现为画面全黑，而不是报错）。退回世界 up 保住一组合法基。
        const math::Vec3 r = math::cross(f, up);
        return math::lengthSq(r) > 1e-12f ? math::normalize(r)
                                          : math::normalize(math::cross(f, worldUp()));
    }

    math::Vec3 target() const noexcept { return position + forward(); }

    // pitch 一律夹紧：控制器与反算路径都走这里，不会漏。
    void setEuler(float yaw, float pitch) noexcept
    {
        yawRadians = yaw;
        pitchRadians = math::clamp(pitch, -kPitchLimit, kPitchLimit);
    }

    math::Mat4 view() const noexcept
    {
        // 与 right() 同样的退化保护：lookAt 内部是 normalize(cross(forward, up))，
        // 共线时交出的是奇异矩阵（几何被压成一条线），这里换成世界 up。
        const math::Vec3 f = forward();
        const math::Vec3 u = math::lengthSq(math::cross(f, up)) > 1e-12f ? up : worldUp();
        return math::lookAt(position, target(), u);
    }

    math::Mat4 projection() const noexcept
    {
        return math::perspective(fovYRadians, aspect, nearPlane, farPlane);
    }

    math::Mat4 viewProjection() const noexcept { return projection() * view(); }

    math::Frustum frustum() const noexcept
    {
        return math::extractFrustum(viewProjection());
    }

    // 由「看向某点」反算 yaw / pitch，用于默认视角与调试。
    // screenUp 只写入 up（画面滚转），不参与 yaw/pitch 的反算 —— 反算永远是
    // 「+Y 为上」的球坐标，与 CameraController 的语义一致。
    static Camera lookingAt(const math::Vec3 &eye, const math::Vec3 &lookTarget,
                            const math::Vec3 &screenUp = {0.0f, 1.0f, 0.0f}) noexcept
    {
        Camera c;
        c.position = eye;
        c.up = screenUp;
        const math::Vec3 d = math::normalize(lookTarget - eye);
        c.pitchRadians = std::asin(math::clamp(d.y, -1.0f, 1.0f));
        c.yawRadians = std::atan2(-d.x, -d.z);
        return c;
    }
};

} // namespace my3d::scene
