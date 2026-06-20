#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

// Generic launcher compiled per-game by veng_add_game with the module filename
// baked in as VENG_GAME_MODULE. Loads the module, calls VengModuleRegister,
// constructs the registered Application, and runs it.
int main(const int argc, char** argv)
{
    // Declared first so it destructs last: the factory closure and type descriptors
    // are code/data in the module image, so the registries and app must be destroyed
    // before the module is unloaded.
    auto module = Veng::ModuleLoader::Load(VENG_GAME_MODULE);
    if (!module)
    {
        Veng::Log::Error("module load failed: {}", module.error());
        return 1;
    }

    Veng::ApplicationRegistry apps;
    Veng::TypeRegistry        types;

    // Builtins must be present before the module registers its types (game components may reference them).
    Veng::RegisterBuiltinTypes(types);

    Veng::VengModuleHost host{.App = apps, .Types = types, .Editor = nullptr};
    module->Register(host);

    Veng::Unique<Veng::Application> app = apps.Create(types);
    VE_ASSERT(app, "module registered no Application");
    app->Run(Veng::vector<Veng::string>(argv, argv + argc));

    return 0;
}
