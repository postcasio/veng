// The same minimal module as test_module.cpp, but its target forces a wrong
// VENG_MODULE_ABI_VERSION (-DVENG_MODULE_ABI_VERSION=999999u), so its exported
// VengModuleAbiVersion disagrees with the engine's. Proves the loader's
// handshake rejects a stale module without ever calling its entry.

#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Module/Module.h>
#include <Veng/Reflection/TypeRegistry.h>

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
    // Never reached: the loader rejects this module on the version handshake. If
    // it ever runs, the test fails loudly.
    VE_ASSERT(false, "wrong-version module entry must never be called");
    host->App.RegisterApplication(
        [](Veng::TypeRegistry& types)
        { return Veng::Unique<Veng::Application>(new ProbeApp(types)); });
}
