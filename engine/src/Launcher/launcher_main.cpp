#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>

// The generic veng launcher: one main shipped by veng, compiled per-game by
// veng_add_game with the module's file name baked in as VENG_GAME_MODULE. It
// loads that module beside the launcher binary (the dynamic loader resolves the
// bare name via the @loader_path/$ORIGIN rpath veng_add_game sets), runs its
// VengModuleRegister entry, constructs the app the module registered, and runs it.
int main(const int argc, char** argv)
{
    // module is declared FIRST so it destructs LAST: a registered Application
    // factory is a closure whose code lives in the module image, so the registry
    // (and the constructed app) must be destroyed before the module is unloaded —
    // reverse declaration order guarantees that.
    auto module = Veng::ModuleLoader::Load(VENG_GAME_MODULE);
    if (!module)
    {
        Veng::Log::Error("module load failed: {}", module.error());
        return 1;
    }

    Veng::ApplicationRegistry apps;
    Veng::VengModuleHost host{.App = apps, .Editor = nullptr};
    module->Register(host);

    Veng::Unique<Veng::Application> app = apps.Create();
    VE_ASSERT(app, "module registered no Application");
    app->Run(Veng::vector<Veng::string>(argv, argv + argc));

    return 0;
}
