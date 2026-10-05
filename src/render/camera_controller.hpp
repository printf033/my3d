#pragma once

// my3d::render —— 相机手感：一份实现，多种输入。

#include "scene/camera.hpp"

namespace my3d::render
{

// 语义动作，不是按键码。
//
// SDL 与 TTY 各自把按键翻译成这个结构，相机手感因此只有一份实现 ——
// 终端里用 hjkl 漫游与在窗口里用 WASD 漫游，走的是同一段代码。
//
// 符号约定（用直觉，不用数学）：
//   yawDelta   > 0  => 视线向右转
//   pitchDelta > 0  => 视线上抬
//   fovDelta   > 0  => 拉近（视场角变小）
struct CameraAction
{
    float forward = 0.0f;   // [-1, 1]
    float strafe = 0.0f;
    float lift = 0.0f;
    float yawDelta = 0.0f;    // 任意单位增量，由 lookSpeed 换算成弧度
    float pitchDelta = 0.0f;
    float fovDelta = 0.0f;
    float speedScale = 1.0f;
};

class CameraController
{
public:
    void apply(const CameraAction &action, double deltaSeconds);

    scene::Camera &camera() noexcept { return camera_; }
    const scene::Camera &camera() const noexcept { return camera_; }

    void setMoveSpeed(float unitsPerSecond) noexcept { moveSpeed_ = unitsPerSecond; }
    void setLookSpeed(float radiansPerUnit) noexcept { lookSpeed_ = radiansPerUnit; }

private:
    scene::Camera camera_;
    float moveSpeed_ = 10.0f;
    float lookSpeed_ = 0.0025f;
    float fovSpeed_ = 1.5f;
};

} // namespace my3d::render
