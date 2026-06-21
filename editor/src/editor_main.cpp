#include <VengEditor/EditorHost.h>

#include <Veng/Application.h>

#include "CookSession.h"

// Editor launcher: compiled per-game by veng_add_editor with module names baked in.
// Modules are resolved beside the binary via the @loader_path/$ORIGIN rpath.
int main(const int argc, char** argv)
{
    const VengEditor::EditorHostInfo info{
        .GameModulePath = Veng::ExecutableDirectory() / VENG_EDITOR_GAME_MODULE,
#ifdef VENG_EDITOR_EDITOR_MODULE
        .EditorModulePath = Veng::ExecutableDirectory() / VENG_EDITOR_EDITOR_MODULE,
#endif
#ifdef VENG_EDITOR_ASSET_MANIFEST
        .AssetManifestPath =
            Veng::path(VENG_EDITOR_ASSET_MANIFEST), // baked absolute by veng_add_editor
#endif
        .App =
            {
                .Name = "veng Editor",
                .InternalRenderExtent = {1280, 720},
                .WindowInfo =
                    {
                        .Extent = {1600, 900},
                        .Resizable = true,
                        .EventCallback = [](Veng::Event&) {},
                        .Title = "veng — Editor",
                        .CaptureMouse = false,
                    },
                .PipelineCachePath = Veng::ExecutableDirectory() / "editor_pipeline_cache.bin",
            },
        // CookSession is stateless; build one per request.
        .Cook = [](const VengEditor::CookRequest& request, Veng::TaskSystem& tasks)
        { return VengEditor::CookSession().Cook(request, tasks); },
    };

    Veng::Unique<VengEditor::EditorHost> host = VengEditor::EditorHost::Create(info);
    host->Run(Veng::vector<Veng::string>(argv, argv + argc));

    return 0;
}
