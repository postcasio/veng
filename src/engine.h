#ifndef _ENGINE_H_
#define _ENGINE_H_

#include <vector>
#include <optional>

#include "gfxcommon.h"
#include "model.h"
#include "object.h"

#include "perspective_camera.h"
#include "window.h"
#include "camera_controller.h"
#include "texture_cache.h"
#include "material_cache.h"
#include "renderer.h"

class Engine
{
public:
    void run();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) = delete;
    Engine &operator=(Engine &&) = delete;
    Window window;

    static auto current()
    {
        static Engine current;

        return &current;
    }

    float currentTime = 0;

    int getOutputWidth();
    int getOutputHeight();

    std::shared_ptr<Object> testObject;

    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<TextureCache> textureCache;
    std::unique_ptr<MaterialCache> materialCache;

    std::shared_ptr<CameraController> cameraController;

    Scene scene;

    PerspectiveCamera camera;

private:
    Engine();

    void initialize();

    void mainLoop();
    void dispose();
};

Engine *engine();

#endif