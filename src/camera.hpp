#pragma once

#include <filament/Camera.h>
#include <math/vec3.h>

struct Camera
{
    double yawFactor = 0.002;
    double pitchFactor = 0.002;
    double zoomFactor = 10;
    double moveFactor = 100;
    double yawRad = -0.5 * std::numbers::pi;
    double pitchRad = 0;
    double fovY = 45;
    double aspect = (19.0 / 6.0);
    double nearLimit = 0.1;
    double farLimit = 1000;
    filament::math::double3 position{0, 0, 5};
    filament::math::double3 front{0, 0, -1};
};
