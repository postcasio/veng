#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>

#ifdef VENG_LAUNCHER_MCP
#include <Veng/Mcp/McpClientCli.h>

#include <iostream>

namespace
{
    // The launcher's own basename (from argv[0]), attributing a --connect error line to the
    // exe the user ran rather than to a generic label.
    Veng::string ProgramName(const char* arg0)
    {
        const Veng::string_view full{arg0 != nullptr ? arg0 : ""};
        const Veng::usize slash = full.find_last_of("/\\");
        return Veng::string(slash == Veng::string_view::npos ? full : full.substr(slash + 1));
    }
}
#endif

// Generic launcher emitted by veng_add_game; VENG_GAME_MODULE is baked in at compile time.
int main(const int argc, char** argv)
{
#ifdef VENG_LAUNCHER_MCP
    // --connect=<port|host:port> makes the launcher a client of an already-running MCP
    // server, not a game: it drives one tool call and exits before the module is loaded.
    const Veng::vector<Veng::string> args(argv, argv + argc);
    for (Veng::usize i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--connect" || args[i].starts_with("--connect="))
        {
            const Veng::vector<Veng::string> clientArgs(args.begin() + 1, args.end());
            return Veng::Mcp::RunClientCli(clientArgs, std::cout, std::cerr, ProgramName(argv[0]));
        }
    }
#endif

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
    Veng::TypeRegistry types;
    Veng::SystemRegistry systems;
    Veng::GuiDriverRegistry drivers;

    // Builtins must be present before the module registers its own: game components may reference
    // builtin types, and a level names the builtin systems the engine pre-registers here.
    Veng::RegisterBuiltinTypes(types);
    Veng::RegisterBuiltinSystems(systems);

    Veng::VengModuleHost host{
        .App = apps, .Types = types, .Systems = systems, .Drivers = &drivers, .Editor = nullptr};
    module->Register(host);

    Veng::Unique<Veng::Application> app = apps.Create(types, systems);
    VE_ASSERT(app, "module registered no Application");
    // Hand the app the populated driver catalog before Run so its managed viewports drive
    // component-authored GuiOverlay drivers.
    app->SetGuiDriverRegistry(&drivers);

    // The app's exit status becomes the process's, so a supervisor or a script can tell a failed
    // start from a run that completed. An app that never sets one exits 0.
    return app->Run(Veng::vector<Veng::string>(argv, argv + argc));
}
