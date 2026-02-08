#include "logger.hpp"
#include "engine.hpp"
#include "window.hpp"

int main()
{
    Engine engine;
    engine.loadModel("../assets/objects/sphere/sphere.fbx",
                     "../bin/my3d_all_all.filamat");
    engine.asyncVerticesIndices2GPU();
    engine.addModel2Scene("myScene", "sphere", "../assets/objects/sphere/sphere.fbx");
    Window_SDL window;
    window.initWindow(engine);
    while (window.isRunning())
        window.updateWindow(engine);
    return 0;
}