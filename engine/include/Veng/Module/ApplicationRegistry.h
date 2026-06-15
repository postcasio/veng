#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Application;

    // A module registers its Application factory in VengModuleRegister; the host
    // (launcher / editor) reads it back to construct and Run() the app. One app
    // per module — RegisterApplication is fatal if called twice. The caller
    // passes the factory explicitly: it captures exactly the ApplicationInfo
    // (and anything else) it wants, with the capture spelling it chooses.
    // Application is forward-declared; the factory body that constructs the
    // concrete app is written in the module TU, which includes Application.h.
    class VE_API ApplicationRegistry
    {
    public:
        void RegisterApplication(function<Unique<Application>()> factory);

        [[nodiscard]] bool HasApplication() const;        // a factory was registered
        [[nodiscard]] Unique<Application> Create() const; // construct it, or nullptr

    private:
        function<Unique<Application>()> m_Factory;
    };
}
