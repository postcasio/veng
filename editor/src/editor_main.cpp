#include <VengEditor/EditorHost.h>

#include <Veng/Application.h>

#include "CookSession.h"

// The veng editor launcher: one main shipped by libveng_editor, compiled per-game
// by veng_add_editor with the game (and optional editor) module file names baked
// in. It resolves those modules beside the editor binary via the @loader_path /
// $ORIGIN rpath veng_add_editor sets, constructs the EditorHost, and runs it.
int main(const int argc, char** argv)
{
    VengEditor::EditorHostInfo info{
        .GameModulePath = Veng::ExecutableDirectory() / VENG_EDITOR_GAME_MODULE,
#ifdef VENG_EDITOR_EDITOR_MODULE
        .EditorModulePath = Veng::ExecutableDirectory() / VENG_EDITOR_EDITOR_MODULE,
#endif
#ifdef VENG_EDITOR_ASSET_MANIFEST
        // The in-tree source manifest, baked absolute by veng_add_editor, so the
        // editor resolves an AssetId to its per-asset JSON source for editing.
        .AssetManifestPath = Veng::path(VENG_EDITOR_ASSET_MANIFEST),
#endif
        .App = {
            .Name = "veng Editor",
            .InternalRenderExtent = {1280, 720},
            .WindowInfo = {
                .Extent = {1600, 900},
                .Resizable = true,
                .EventCallback = [](Veng::Event&) {},
                .Title = "veng — Editor",
                .CaptureMouse = false,
            },
#ifdef VENG_EDITOR_FONTS_DIR
            .ImGui = Veng::ImGuiLayerInfo{
                .DefaultFontPath = Veng::path(VENG_EDITOR_FONTS_DIR) / "Roboto.ttf",
            },
#endif
            .PipelineCachePath = Veng::ExecutableDirectory() / "editor_pipeline_cache.bin",
        },
        // The cook-on-demand backend: links libveng_cook (the importer table),
        // which libveng_editor never does. CookSession is stateless, so the
        // backend builds one per request.
        .Cook = [](const VengEditor::CookRequest& request, Veng::TaskSystem& tasks)
        {
            return VengEditor::CookSession().Cook(request, tasks);
        },
    };

    Veng::Unique<VengEditor::EditorHost> host = VengEditor::EditorHost::Create(info);
    host->Run(Veng::vector<Veng::string>(argv, argv + argc));

    return 0;
}
