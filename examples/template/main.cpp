#include <Veng/Application.h>
#include <Veng/Module/Module.h>

using namespace Veng;

// The smallest veng game: open a window and draw one slowly rotating lit cube. The world is
// authored entirely as data — a cooked Level (named by the project's startup level) that
// references a world Prefab (a camera, a directional light, and a cube whose mesh is an inline
// CubeShape recipe and which carries a ConstantMotion the engine's ConstantMotionSystem spins).
// The engine bootstraps and drives it: it reads the cooked project, mounts the packs it names,
// loads the startup level, owns the running scene + simulation, ticks the level's system set (the
// builtin systems the host pre-registers), and pushes the resolved camera into the managed
// viewport every frame. So the whole game is a VengModuleRegister that registers a bare
// Application — no custom component or system, no system registration, no lifecycle or per-frame
// code. Copy this directory to start a new veng game; see examples/hello-triangle for the richer
// surface (custom components and systems, gameplay, the debug UI, build configurations).
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->App.RegisterApplication(
        [](TypeRegistry& types, SystemRegistry& systems)
        {
            return Unique<Application>(new Application(
                ApplicationInfo{
                    .Name = "Template",
                    .WindowInfo =
                        {
                            .Extent = {1280, 720},
                            .Title = "veng — Template",
                        },
                    // The engine owns the primary viewport (its SceneRenderer + the gather +
                    // composite tail) and drives the managed world: it reads the cooked project,
                    // mounts its packs, loads the startup level, ticks the simulation, and pushes
                    // the resolved camera each frame.
                    .ManagedViewport = ManagedViewportInfo{},
                    .World = GameWorldInfo{.Project = "project.vengproj"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
