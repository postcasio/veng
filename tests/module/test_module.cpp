// A minimal game module: exports the ABI version + VengModuleRegister, and on
// registration registers a game component into the host's TypeRegistry, stores a
// trivial Application factory (never invoked here, so no Context/Window is
// constructed), and asserts the host's Editor slot is null.

#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Module/Module.h>
#include <Veng/Reflection/TypeRegistry.h>

#include "probe_component.h"

namespace
{
    class ProbeApp : public Veng::Application
    {
    public:
        explicit ProbeApp(Veng::TypeRegistry& types)
            : Veng::Application(Veng::ApplicationInfo{}, types) {}
    };
}

VE_EXPORT_MODULE_ABI()

extern "C" VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host)
{
    VE_ASSERT(host != nullptr, "host must be non-null");
    VE_ASSERT(host->Editor == nullptr, "a game module is never loaded by the editor here");

    host->Types.Register<Probe>();

    host->App.RegisterApplication(
        [](Veng::TypeRegistry& types)
        { return Veng::Unique<Veng::Application>(new ProbeApp(types)); });
}
