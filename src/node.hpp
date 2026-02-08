#pragma once

#include "mesh.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

struct Node
{
    filament::Box bound;
    filament::math::mat4f transform;
    std::unordered_map<std::string, Mesh> meshes;
    std::unordered_map<std::string, Node> children;
};