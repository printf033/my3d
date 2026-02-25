#include "logger.hpp"
#include "engine.hpp"
#include "window.hpp"

int main()
{
    Engine engine;
    std::string model = "../assets/model/mia/scene.gltf";
    std::string filamat = "../assets/shader/desktop/vulkan/lit.filamat";
    std::string skybox = "../assets/IBL/sky/cmgen/skybox.ktx";
    std::string ibl = "../assets/IBL/sky/cmgen/ibl.ktx";
    engine.loadModel(model, filamat);
    engine.asyncVerticesIndices2GPU();
    engine.addModel2Scene("myScene", "myModel", model);
    engine.addSkybox2Scene("myScene", skybox);
    engine.addIBL2Scene("myScene", ibl, 30000.0f);

    auto light = engine.getEntity("light");
    filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
        .color(filament::Color::toLinear<filament::ACCURATE>(filament::sRGBColor(0.98f, 0.92f, 0.89f))) 
        .intensity(110000.0f)                                                                          
        .direction({0.0f, -1.0f, -1.0f})                                                                
        .castShadows(true)                                                                            
        .build(*engine.getEngine(), light);
    auto scene = engine.getScene("myScene");
    scene->addEntity(light);

    Window_SDL window;
    window.initWindow(engine);
    while (window.isRunning())
        window.updateWindow(engine);
    return 0;
}