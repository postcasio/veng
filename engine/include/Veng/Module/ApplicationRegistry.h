#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Application;
    class TypeRegistry;

    /// @brief Holds the Application factory a module registers during VengModuleRegister.
    ///
    /// The host (launcher / editor) reads the factory back via Create() to construct
    /// and Run() the app. One app per module — RegisterApplication is fatal if called twice.
    /// The host-owned TypeRegistry is forwarded to the Application constructor at Create time.
    class VE_API ApplicationRegistry
    {
    public:
        /// @brief Stores the factory; fatal if called more than once per registry.
        /// @param factory  Callable that constructs the concrete Application given the TypeRegistry.
        void RegisterApplication(function<Unique<Application>(TypeRegistry&)> factory);

        /// @brief Returns true if a factory has been registered.
        [[nodiscard]] bool HasApplication() const;

        /// @brief Constructs and returns the Application using the registered factory.
        /// @param types  Host-owned TypeRegistry forwarded to the Application constructor.
        /// @return The newly constructed Application, or nullptr if no factory is registered.
        [[nodiscard]] Unique<Application> Create(TypeRegistry& types) const;

    private:
        /// @brief The registered factory, or empty.
        function<Unique<Application>(TypeRegistry&)> m_Factory;
    };
}
