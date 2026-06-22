#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    /// @brief A registered system's catalog entry: its identity and how to build it.
    ///
    /// Holds the SystemId and display name read off the system's VengSystem trait at
    /// registration, so the catalog enumerates the available systems without
    /// instantiating any.
    struct SystemEntry
    {
        /// @brief The system's stable identity, the catalog key.
        SystemId Id = InvalidSystemId;
        /// @brief The system's display name (logs/editor display).
        string Name;
        /// @brief Default-constructs one instance of the system.
        function<Unique<SceneSystem>()> Factory;
    };

    /// @brief The catalog of SceneSystems a module registers during VengModuleRegister.
    ///
    /// Host-owned and borrowed, mirroring the TypeRegistry and ApplicationRegistry:
    /// the host (launcher or editor) constructs it, threads it through VengModuleHost,
    /// and the module registers its systems into it. Each Register<T>() reads the
    /// system's SystemId + name off its VengSystem trait and stores them beside the
    /// factory, so a consumer enumerates the available systems and resolves an id to a
    /// factory without instantiating anything. A SceneSimulation is built from an
    /// ordered SystemId set selecting catalog entries, or from every entry. Registration
    /// is GPU-free — constructing a system touches no Context/device, preserving the
    /// headless/cooker contract.
    class VE_API SystemRegistry
    {
    public:
        /// @brief Registers system type T into the catalog under its authored SystemId.
        ///
        /// Reads SystemIdOf<T>() and the display name off T's VengSystem trait (so a
        /// system without a VE_SYSTEM fails to compile here), and stores
        /// `{ SystemId, Name, factory }`. Registration order is the order Entries()
        /// reports and the "all registered" convenience runs.
        /// @tparam T The concrete SceneSystem subclass to register.
        /// @pre No other system already claims T's SystemId.
        /// @warning Registering two systems under the same SystemId is a fatal collision assert.
        template <class T>
        void Register()
        {
            constexpr SystemId id = SystemIdOf<T>();
            static_assert(id != InvalidSystemId,
                          "VengSystem<T>::Id must be a non-zero authored id");

            for (const SystemEntry& entry : m_Entries)
            {
                VE_ASSERT(entry.Id != id,
                          "SystemId collision: '{}' and '{}' both claim SystemId {:#018x}",
                          SystemNameOf<T>(), entry.Name, id);
            }

            m_Entries.emplace_back(SystemEntry{
                .Id = id,
                .Name = SystemNameOf<T>(),
                .Factory = [] { return Unique<SceneSystem>(new T()); },
            });
        }

        /// @brief Read-only view over the catalog entries, in registration order.
        ///
        /// Each entry carries its SystemId, display name, and factory — the catalog a
        /// consumer enumerates to list the available systems without instantiating any.
        /// @return The registered entries, in registration order.
        [[nodiscard]] const vector<SystemEntry>& Entries() const { return m_Entries; }

        /// @brief Resolves a SystemId to the system it builds.
        ///
        /// @param id  The SystemId to resolve.
        /// @return A freshly built system, or nullptr if no entry claims the id.
        [[nodiscard]] Unique<SceneSystem> Instantiate(SystemId id) const;

        /// @brief Instantiates one of each registered system, in registration order.
        ///
        /// The "all registered" convenience a SceneSimulation uses when no explicit
        /// system set is named.
        /// @return The freshly built systems, owned by the caller, in registration order.
        [[nodiscard]] vector<Unique<SceneSystem>> Instantiate() const;

        /// @brief Returns the number of registered systems.
        [[nodiscard]] usize Count() const;

    private:
        /// @brief The registered catalog entries, in registration order.
        vector<SystemEntry> m_Entries;
    };
}
