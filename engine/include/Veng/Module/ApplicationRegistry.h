#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Application;
    class TypeRegistry;
    class SystemRegistry;

    /// @brief Holds the Application factory a module registers during VengModuleRegister.
    ///
    /// The host (launcher / editor) reads the factory back via Create() to construct
    /// and Run() the app. One app per module — RegisterApplication is fatal if called twice.
    /// The host-owned TypeRegistry and SystemRegistry are forwarded to the Application
    /// constructor at Create time.
    class VE_API ApplicationRegistry
    {
    public:
        /// @brief Stores the factory; fatal if called more than once per registry.
        /// @param factory  Callable that constructs the concrete Application given the
        ///                 host-owned TypeRegistry and SystemRegistry.
        void
        RegisterApplication(function<Unique<Application>(TypeRegistry&, SystemRegistry&)> factory);

        /// @brief Returns true if a factory has been registered.
        [[nodiscard]] bool HasApplication() const;

        /// @brief Constructs and returns the Application using the registered factory.
        /// @param types    Host-owned TypeRegistry forwarded to the Application constructor.
        /// @param systems  Host-owned SystemRegistry forwarded to the Application constructor.
        /// @return The newly constructed Application, or nullptr if no factory is registered.
        [[nodiscard]] Unique<Application> Create(TypeRegistry& types,
                                                 SystemRegistry& systems) const;

    private:
        /// @brief The registered factory, or empty.
        function<Unique<Application>(TypeRegistry&, SystemRegistry&)> m_Factory;
    };
}
