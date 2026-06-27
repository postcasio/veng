#include <VengEditor/EditorHost.h>

#include <Veng/Application.h>

#include "CookSession.h"

// veng-editor: the single, project-agnostic editor shell. Launched with a project; it reads the
// module(s) the project names (ProjectSettings::Module / EditorModule) and dlopens them from the
// project's build-output dir. --project names the source project.veng to open. The build dir is
// normally discovered from the project's .veng/build.json sidecar (so a launcher can spawn the
// editor on a project with no extra args); --build-dir overrides that discovery, and with neither
// the editor falls back to its own directory (the relocatable ship layout).
int main(const int argc, char** argv)
{
    const Veng::vector<Veng::string> args(argv, argv + argc);
    Veng::optional<Veng::path> projectPath;
    Veng::optional<Veng::path> buildDir;
    for (Veng::usize i = 1; i < args.size(); ++i)
    {
        if ((args[i] == "--project" || args[i] == "-p") && i + 1 < args.size())
        {
            projectPath = Veng::path(args[++i]);
        }
        else if (args[i] == "--build-dir" && i + 1 < args.size())
        {
            buildDir = Veng::path(args[++i]);
        }
    }

    const VengEditor::EditorHostInfo info{
        .ProjectPath = projectPath,
        .BuildDir = buildDir,
        .App =
            {
                .Name = "veng Editor",
                .HeadlessExtent = {1280, 720},
                .WindowInfo =
                    {
                        .Extent = {1600, 900},
                        .Resizable = true,
                        .Title = "veng — Editor",
                        .CaptureMouse = false,
                    },
                .PipelineCachePath = Veng::ExecutableDirectory() / "editor_pipeline_cache.bin",
            },
        // CookSession is stateless; build one per request.
        .Cook = [](const VengEditor::CookRequest& request, Veng::TaskSystem& tasks)
        { return VengEditor::CookSession().Cook(request, tasks); },
    };

    // The editor consumes its own --project/--build-dir flags above; Application::Run reads only a
    // launcher-convention working-directory arg, so hand it just the program name (the editor
    // resolves the project and build dir as absolute paths, needing no working-directory selector).
    Veng::Unique<VengEditor::EditorHost> host = VengEditor::EditorHost::Create(info);
    host->Run({args.front()});

    return 0;
}
