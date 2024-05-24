#include "gfxcommon.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>
#include <chrono>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "engine.h"
#include "fs.h"
#include "transform_uniforms.h"
#include "mesh.h"
#include "model.h"
#include "perspective_camera.h"
#include "point_light.h"
#include <nlohmann/json.hpp>

Engine *engine()
{
    return Engine::current();
}

void Engine::run(void)
{
    initialize();
    stbi_set_flip_vertically_on_load(true);

    camera = PerspectiveCamera(45.0f, getOutputWidth() / (float)getOutputHeight(), 0.1f, 4096.0f);
    camera.position = glm::vec3(0.0f, 32.0f, 0.0f);
    camera.lookAt(glm::vec3(0.0f, 64.0f, 0.0f));

    cameraController = std::make_shared<CameraController>(&camera, &window);

    testObject = std::make_shared<Object>(Model::fromPath("maps/indoortest.obj"));

    scene.add(testObject);

    auto entities = nlohmann::json::parse(readFile("maps/indoortest-compile.json"));

    for (auto &entity : entities)
    {
        auto classname = entity["classname"].get<std::string>();

        if (classname == "light_point")
        {
            auto light = std::make_shared<PointLight>();
            auto origin = entity["origin"].get<std::vector<float>>();
            light->position.x = origin[0];
            light->position.y = origin[1];
            light->position.z = origin[2];
            auto color = entity["color"].get<std::vector<float>>();
            light->color.x = color[0];
            light->color.y = color[1];
            light->color.z = color[2];

            if (entity.contains("intensity"))
            {
                light->intensity = entity["intensity"].get<float>();
            }
            if (entity.contains("constant"))
            {
                light->constant = entity["constant"].get<float>();
            }
            if (entity.contains("linear"))
            {
                light->linear = entity["linear"].get<float>();
            }
            if (entity.contains("quadratic"))
            {
                light->quadratic = entity["quadratic"].get<float>();
            }
            light->matrixDirty = true;

            std::cout << "Made light at " << light->position.x << ", " << light->position.y << ", " << light->position.z << " with color " << light->color.x << ", " << light->color.y << ", " << light->color.z << std::endl;
            // scene.add(light);
        }
    }

    light = std::make_shared<PointLight>();
    light->position = glm::vec3(250.0f, 650.0f, 32.0f);
    light->color = glm::vec3(1.0f, 0.7f, 0.7f);
    light->linear = 0.0006;
    light->quadratic = 0.000008;
    scene.add(light);

    mainLoop();

    dispose();
}

void Engine::initialize()
{
    window.createWindow(this);

    renderer = std::make_unique<Renderer>();

    renderer->initialize();

    textureCache = std::make_unique<TextureCache>(256);
    materialCache = std::make_unique<MaterialCache>(1024);
}

void Engine::mainLoop()
{
    while (!window.shouldClose())
    {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto time = std::chrono::high_resolution_clock::now();

        currentTime = std::chrono::duration<float, std::chrono::seconds::period>(time - startTime).count();

        mouse_state blankState = {
            {},
        };
        window.setMouseState(blankState);

        window.pollEvents();

        window.updateKeyboardState();

        cameraController->updateCamera();

        light->position = camera.position;
        light->translateZ(32.0f);
        light->matrixDirty = true;

        // scene.children[1]->position.y = 64.0f + 64.0f * sin(currentTime / 4.0);
        // scene.children[1]->position.x = -1000.0f + 1000.0f * cos(currentTime / 4.0);
        // scene.children[1]->position.z = 8.0f + 64.0f * sin(currentTime / 4.0);

        // scene.children[1]->matrixDirty = true;

        renderer->drawFrame();
    }

    renderer->device->waitIdle();
}

void Engine::dispose()
{
    // force all the vertex/index/uniform buffers to be cleaned up
    scene.children.clear();
    testObject.reset();

    textureCache.reset();
    materialCache.reset();

    renderer->dispose();
    renderer.reset();

    window.destroy();

    std::cout << "shut down complete" << std::endl;
}

Engine::Engine() : camera(45.0f, 1.0f, 0.1f, 10.0f)
{
}

int Engine::getOutputWidth()
{
    return renderer->swapChain->extent.width;
}

int Engine::getOutputHeight()
{
    return renderer->swapChain->extent.height;
}
