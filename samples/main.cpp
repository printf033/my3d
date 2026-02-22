#include "logger.hpp"
#include "engine.hpp"
#include "window.hpp"

int main()
{
    Engine engine;
    std::string model = "../bin/assets/model/buildings/All.fbx";
    std::string filamat = "../bin/unlit_all_all.filamat";
    std::string skybox = "../bin/assets/IBL/sky/cmgen/sky_skybox.ktx";
    std::string ibl = "../bin/assets/IBL/sky/cmgen/sky_ibl.ktx";
    engine.loadModel(model, filamat);
    engine.asyncVerticesIndices2GPU();
    engine.addModel2Scene("myScene", "myModel", model);
    engine.addSkybox2Scene("myScene", skybox);
    // engine.addIBL2Scene("myScene", ibl, 30000.0f);
    Window_SDL window;
    window.initWindow(engine);
    while (window.isRunning())
        window.updateWindow(engine);
    return 0;
}