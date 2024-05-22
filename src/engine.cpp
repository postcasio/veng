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

    light = std::make_shared<PointLight>();
    light->position = glm::vec3(0.0f, 64.0f, 0.0f);
    light->color = glm::vec3(0.2, 0.6, 0.9);

    scene.add(light);
    light2 = std::make_shared<PointLight>();
    light2->position = glm::vec3(0.0f, 64.0f, -256.0f);
    light2->color = glm::vec3(0.9, 0.2, 0.2);
    light2->linear = 0.0005f;
    light2->quadratic = 0.00005f;
    scene.add(light2);
    // auto light2 = std::make_shared<PointLight>();
    // light2->position = glm::vec3(250.0f, 650.0f, 32.0f);
    // light2->color = glm::vec3(1.0f, 0.2f, 0.2f);
    // scene.add(light2);
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
        light2->position.y = 256.0f + 240.0f * sin(currentTime / 2.0);

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
