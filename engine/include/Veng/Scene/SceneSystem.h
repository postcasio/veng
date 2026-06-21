#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class Input;

    /// @brief Per-tick services handed to every SceneSystem.
    ///
    /// Borrowed for the duration of the call: a system reads from these but does
    /// not own them. The Input reference is the frame-coherent input service, null
    /// in headless mode.
    struct SystemContext
    {
        /// @brief The asset manager a system loads or builds resources through.
        AssetManager& Assets;
        /// @brief The frame-coherent input service; unavailable (null) in headless mode.
        const Input& Input;
    };

    /// @brief A unit of gameplay logic over a Scene, registered via the module host
    /// and ticked by a SceneSimulation.
    ///
    /// Registered into the host-owned SystemRegistry exactly like a reflected type;
    /// the SceneSimulation driver instantiates and ticks the registered systems.
    /// The runtime app and the editor's Play mode tick the same systems, in
    /// registration order.
    class SceneSystem
    {
    public:
        /// @brief Virtual destructor; systems are owned through SceneSystem pointers.
        virtual ~SceneSystem() = default;

        /// @brief Called once when play/simulation begins, before the first OnUpdate.
        ///
        /// The default does nothing.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (assets, input).
        virtual void OnStart(Scene& scene, const SystemContext& context) {}

        /// @brief Called once per frame to advance the system's logic.
        /// @param scene    The scene the system operates over.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (assets, input).
        virtual void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) = 0;

        /// @brief Called once when play/simulation ends, after the last OnUpdate.
        ///
        /// The default does nothing.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (assets, input).
        virtual void OnStop(Scene& scene, const SystemContext& context) {}
    };
}
