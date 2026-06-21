#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class SystemRegistry;

    /// @brief Drives the registered SceneSystems over a Scene.
    ///
    /// The single simulation driver both the runtime app and the editor's Play mode
    /// own. Constructed from a SystemRegistry — it instantiates one of each registered
    /// system at construction and holds them — then Start/Update/Stop each system in
    /// registration order across a play session.
    class SceneSimulation
    {
    public:
        /// @brief Instantiates the registered systems and holds them for the session.
        /// @param registry  Host-owned registry whose factories produce the systems.
        explicit SceneSimulation(const SystemRegistry& registry);

        /// @brief Calls OnStart on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Start(Scene& scene, const SystemContext& context);

        /// @brief Calls OnUpdate on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services forwarded to each system.
        void Update(Scene& scene, f32 delta, const SystemContext& context);

        /// @brief Calls OnStop on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Stop(Scene& scene, const SystemContext& context);

        /// @brief Returns true when no systems were registered.
        [[nodiscard]] bool IsEmpty() const { return m_Systems.empty(); }

    private:
        /// @brief The instantiated systems, in registration (run) order.
        vector<Unique<SceneSystem>> m_Systems;
    };
}
