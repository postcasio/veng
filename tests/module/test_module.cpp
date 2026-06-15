// A minimal game module: exports the ABI version + VengModuleRegister, and on
// registration stores a trivial Application factory (never invoked here, so no
// Context/Window is constructed) and asserts the host's Editor slot is null.

#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Module/Module.h>

namespace
{
    class ProbeApp : public Veng::Application
    {
    public:
        ProbeApp() : Veng::Application(Veng::ApplicationInfo{}) {}
    };
}

VE_EXPORT_MODULE_ABI()

extern "C" VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host)
{
    VE_ASSERT(host != nullptr, "host must be non-null");
    VE_ASSERT(host->Editor == nullptr, "a game module is never loaded by the editor here");

    host->App.RegisterApplication(
        [] { return Veng::Unique<Veng::Application>(new ProbeApp()); });
}
