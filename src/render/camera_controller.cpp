#include "render/camera_controller.hpp"

namespace my3d::render
{

void CameraController::apply(const CameraAction &a, double deltaSeconds)
{
    const float dt = static_cast<float>(deltaSeconds);

    // 视角。CameraAction 用直觉符号（yawDelta > 0 = 右转），而 Camera::yawRadians
    // 增大是绕 +Y 的右手方向（左转），故这里取负。符号换算集中在这一处：
    // 若输入层也翻一次，两处翻转会互相抵消，症状是「按右键向左转」且极难定位。
    camera_.setEuler(camera_.yawRadians - a.yawDelta * lookSpeed_,
                     camera_.pitchRadians + a.pitchDelta * lookSpeed_);

    // 位移：前后 / 左右沿相机基，升降沿世界 up（QE 的直觉）。
    const float speed = moveSpeed_ * a.speedScale * dt;
    if (speed != 0.0f)
    {
        const math::Vec3 f = camera_.forward();
        const math::Vec3 r = camera_.right();
        const math::Vec3 u = scene::Camera::worldUp();

        camera_.position = camera_.position + f * (a.forward * speed) +
                           r * (a.strafe * speed) + u * (a.lift * speed);
    }

    if (a.fovDelta != 0.0f)
    {
        camera_.fovYRadians = math::clamp(camera_.fovYRadians - a.fovDelta * fovSpeed_ * dt,
                                          0.05f, 2.5f);
    }
}

} // namespace my3d::render
