#include "logger.hpp"
#include "engine.hpp"
#include "window.hpp"

int main()
{
    Engine engine;
    std::string model = "../assets/model/mia/scene.gltf";
    std::string filamat = "../assets/shader/desktop/vulkan/unlit.filamat";
    std::string skybox = "../assets/IBL/sky/cmgen/skybox.ktx";
    std::string ibl = "../assets/IBL/sky/cmgen/ibl.ktx";
    engine.loadModel(model, filamat);
    engine.asyncVerticesIndices2GPU();
    engine.addModel2Scene("myScene", "myModel", model);
    engine.addSkybox2Scene("myScene", skybox);
    engine.addIBL2Scene("myScene", ibl, 30000.0f);
    Window_SDL window;
    window.initWindow(engine);
    while (window.isRunning())
        window.updateWindow(engine);
    return 0;
}