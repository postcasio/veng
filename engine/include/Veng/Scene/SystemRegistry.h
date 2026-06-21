#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    /// @brief Holds the SceneSystem factories a module registers during VengModuleRegister.
    ///
    /// Host-owned and borrowed, mirroring the TypeRegistry and ApplicationRegistry:
    /// the host (launcher or editor) constructs it, threads it through VengModuleHost,
    /// and the module registers its systems into it. A SceneSimulation reads the
    /// factories back via Instantiate() to build the running system set. Registration
    /// is GPU-free — constructing a system touches no Context/device, preserving the
    /// headless/cooker contract — and registration order is the run order.
    class VE_API SystemRegistry
    {
    public:
        /// @brief Registers system type T, appending its factory to the run order.
        ///
        /// Each Register<T>() adds a factory that default-constructs a T; Instantiate()
        /// builds one of each registered system in registration order.
        /// @tparam T The concrete SceneSystem subclass to register.
        template <class T>
        void Register()
        {
            m_Factories.emplace_back([] { return Unique<SceneSystem>(new T()); });
        }

        /// @brief Instantiates one of each registered system, in registration order.
        ///
        /// Called once by a SceneSimulation at construction.
        /// @return The freshly built systems, owned by the caller.
        [[nodiscard]] vector<Unique<SceneSystem>> Instantiate() const;

        /// @brief Returns the number of registered system factories.
        [[nodiscard]] usize Count() const;

    private:
        /// @brief The registered factories, in registration (run) order.
        vector<function<Unique<SceneSystem>()>> m_Factories;
    };
}
