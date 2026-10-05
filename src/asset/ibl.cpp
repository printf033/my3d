#include "asset/ibl.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace my3d::asset
{

bool loadSphericalHarmonics(const std::string &path, SphericalHarmonics &out) noexcept
{
    out = SphericalHarmonics{};

    std::ifstream ifs(path);
    if (!ifs)
        return false;

    std::string line;
    int index = 0;
    while (index < 9 && std::getline(ifs, line))
    {
        const size_t start = line.find('(');
        const size_t end = line.find(')');
        if (start == std::string::npos || end == std::string::npos || end <= start)
            continue;

        const std::string values = line.substr(start + 1, end - start - 1);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (std::sscanf(values.c_str(), "%f, %f, %f", &r, &g, &b) == 3)
        {
            out.bands[static_cast<size_t>(index)] = math::Vec3{r, g, b};
            ++index;
        }
    }

    out.valid = (index == 9);
    return out.valid;
}

} // namespace my3d::asset
