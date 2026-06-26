#include <Veng/Application.h>
#include <Veng/Module/Module.h>
#include <Veng/Scene/Motion.h>

using namespace Veng;

// The smallest veng game: open a window and draw one slowly rotating lit cube. The world is
// authored entirely as data — a cooked Level (named by the pack's startupLevel header) that
// references a world Prefab (a camera, a directional light, and a cube whose mesh is an inline
// CubeShape recipe and which carries a ConstantMotion to spin). The engine bootstraps and drives
// it: it mounts the pack, loads the startup level, owns the running scene + simulation, ticks the
// level's system set, and pushes the resolved camera into the managed viewport every frame. The
// only registration is selecting the engine's ConstantMotionSystem the level names — there is no
// custom component or system and no lifecycle or per-frame code. Copy this directory to start a new
// veng game; see examples/hello-triangle for the richer surface (custom components and systems,
// gameplay, the debug UI, build configurations).
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The level's system set names ConstantMotionSystem; register it so the simulation resolves it
    // and the authored cube spins. An id the registry lacks is silently skipped.
    host->Systems.Register<ConstantMotionSystem>();

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
                    // composite tail) and drives the managed world: it mounts the pack, loads the
                    // startup level, ticks the simulation, and pushes the resolved camera each frame.
                    .ManagedViewport = ManagedViewportInfo{},
                    .World = GameWorldInfo{.AssetPack = "template.vengpack"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
