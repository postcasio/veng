#include <VengEditor/EditorHost.h>

#include <Veng/Application.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Log.h>

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Scene/Scene.h>

#include <cstdio>
#include <cstdlib>

#include "AssetEditorPanel.h"
#include "AssetSourceIndex.h"
#include "CookSession.h"
#include "EditorMcp.h"

#ifndef VENG_EDITOR_VERSION
#define VENG_EDITOR_VERSION "unknown"
#endif

namespace
{
    using namespace Veng;

    /// @brief Parsed --mcp flags: whether the server is on, its port, and the write gate.
    struct McpOptions
    {
        /// @brief Whether --mcp was given; the server is constructed only when true.
        bool Enabled = false;
        /// @brief The requested port (0 picks an ephemeral one), from --mcp=<port>.
        u16 Port = 0;
        /// @brief Whether --mcp-write was given, enabling the mutation tools.
        bool AllowMutations = false;
    };

    /// @brief Parses the --mcp[=port] / --mcp-write flags out of the argument list.
    McpOptions ParseMcpOptions(const vector<string>& args)
    {
        McpOptions options;
        for (const string& arg : args)
        {
            if (arg == "--mcp")
            {
                options.Enabled = true;
            }
            else if (arg.starts_with("--mcp="))
            {
                options.Enabled = true;
                const string value = arg.substr(6);
                options.Port = static_cast<u16>(std::strtoul(value.c_str(), nullptr, 10));
            }
            else if (arg == "--mcp-write")
            {
                options.AllowMutations = true;
            }
        }
        return options;
    }

    /// @brief The MCP server plus the editor-side state its closures capture, owned by main.
    ///
    /// Its members outlive the McpServer (declared last, torn down first), so the host closures the
    /// server and the editor tools capture by reference stay valid for the server's lifetime. The
    /// cook-status map is render-thread-only (updated by RequestCook's main-thread continuation, read
    /// by the cook-status tool during Pump), so it needs no lock. Both host structs hold reference
    /// members, so they are built in the constructor's init list — not default-constructed.
    class EditorMcpService
    {
    public:
        /// @brief Wires both host seams to the editor host, backed by this service's own state.
        explicit EditorMcpService(VengEditor::EditorHost& host)
            : m_EditorHost{.Types = host.GetTypeRegistry(),
                           .Panels = [&host] { return host.GetOpenPanels(); },
                           .FocusedDocument = [&host] { return host.GetFocusedDocument(); },
                           .DocumentScene = [&host] { return FocusedDocumentScene(host); },
                           .AssetSources = [&host] { return host.GetAssetSources(); },
                           .OpenAsset = [&host](AssetId id) { return OpenAsset(host, id); },
                           .SetPanelVisible = [&host](string_view title, bool visible)
                           { return host.SetPanelVisible(title, visible); },
                           .PanelViewport = [&host](string_view title)
                           { return host.GetPanelViewport(title); },
                           .RequestCook = [this, &host](AssetId id) -> VoidResult
                           { return RequestCook(host, id); },
                           .CookStatus = [this](AssetId id) -> optional<string>
                           {
                               const auto it = m_CookStatus.find(id.Value);
                               return it != m_CookStatus.end() ? optional<string>(it->second)
                                                               : std::nullopt;
                           }},
              m_Host{.Types = host.GetTypeRegistry(),
                     .Assets = host.GetAssetManager(),
                     .CurrentWorld = [&host] { return FocusedDocumentScene(host); },
                     .Viewport = [&host](string_view name) { return host.GetPanelViewport(name); },
                     .ViewportNames = [&host] { return host.GetSceneViewportNames(); },
                     .ApplyMutation = [this](const Mcp::McpMutation& mutation)
                     { return VengEditor::ApplyEditorMutation(m_EditorHost, mutation); }}
        {
        }

        /// @brief Constructs the server, registers every tool family, and installs the frame pump.
        void Start(VengEditor::EditorHost& host, const McpOptions& options)
        {
            Mcp::McpServerInfo info;
            info.ServerName = "veng-editor";
            info.Port = options.Port;
            info.AllowMutations = options.AllowMutations;

            m_Server = Mcp::McpServer::Create(info, m_Host);
            VengEditor::RegisterEditorReflectionTools(*m_Server, m_EditorHost);
            VengEditor::RegisterEditorHostTools(*m_Server, m_EditorHost);

            // The world/render/mutation tools auto-register from the McpHost at Create; the editor
            // tools register above. Drive Pump() each frame at the host's scene-safe point.
            Mcp::McpServer* server = m_Server.get();
            host.SetFramePump([server] { server->Pump(); });

            // The console panel's sink owns Log output by now (Start runs after OnInitialize built
            // the panels), so the readiness line goes straight to stdout too: it is the
            // machine-readable contract an attaching agent or the conformance test parses for the
            // bound port. The Log::Info shows the same line in the editor's own console.
            std::printf("veng-editor: MCP server listening on 127.0.0.1:%u%s\n",
                        static_cast<unsigned>(m_Server->GetPort()),
                        options.AllowMutations ? " (writes enabled)" : " (read-only)");
            std::fflush(stdout);
            Log::Info("veng-editor: MCP server listening on 127.0.0.1:{}{}", m_Server->GetPort(),
                      options.AllowMutations ? " (writes enabled)" : " (read-only)");
        }

    private:
        /// @brief The focused document's edit scene, or null when none is focused / it has no scene.
        static Scene* FocusedDocumentScene(VengEditor::EditorHost& host)
        {
            VengEditor::AssetEditorPanel* doc = host.GetFocusedDocument();
            return doc != nullptr ? doc->GetDocumentScene() : nullptr;
        }

        /// @brief Resolves an id's type through the index and opens its editor; false when unknown.
        static bool OpenAsset(VengEditor::EditorHost& host, AssetId id)
        {
            const VengEditor::AssetSourceIndex* sources = host.GetAssetSources();
            if (sources == nullptr)
            {
                return false;
            }
            const VengEditor::AssetSourceIndex::Entry* entry = sources->Find(id);
            if (entry == nullptr || !host.HasAssetEditor(entry->Type))
            {
                return false;
            }
            host.OpenAssetEditor(entry->Type, id);
            return true;
        }

        /// @brief Kicks a cook of the asset and records its status; fire-and-poll (never blocks).
        VoidResult RequestCook(VengEditor::EditorHost& host, AssetId id)
        {
            const VengEditor::AssetSourceIndex* sources = host.GetAssetSources();
            const VengEditor::AssetSourceIndex::Entry* entry =
                sources != nullptr ? sources->Find(id) : nullptr;
            if (entry == nullptr)
            {
                return std::unexpected(fmt::format("asset {} is unknown to the project", id.Value));
            }
            m_CookStatus[id.Value] = "running";
            host.RequestCook(VengEditor::CookRequest{.SourcePath = entry->Source,
                                                     .TargetId = id,
                                                     .Type = entry->Type},
                             [this, key = id.Value](Result<MountHandle> result)
                             { m_CookStatus[key] = result ? string("ok") : result.error(); });
            return {};
        }

        VengEditor::EditorMcpHost m_EditorHost;
        Mcp::McpHost m_Host;

        /// @brief Latest per-asset cook status (running / ok / error), keyed by AssetId value.
        unordered_map<u64, string> m_CookStatus;

        /// @brief The server, declared last so it is torn down before the state its handlers touch.
        Unique<Mcp::McpServer> m_Server;
    };

    /// @brief Constructs the MCP service over the host and starts its server behind --mcp.
    Unique<EditorMcpService> StartMcp(VengEditor::EditorHost& host, const McpOptions& options)
    {
        auto service = CreateUnique<EditorMcpService>(host);
        service->Start(host, options);
        return service;
    }
}

// veng-editor: the single, project-agnostic editor shell. Launched with a project; it reads the
// module(s) the project names (ProjectSettings::Module / EditorModule) and dlopens them from the
// project's build-output dir. --project names the source project.veng to open. The build dir is
// normally discovered from the project's .veng/build.json sidecar (so a launcher can spawn the
// editor on a project with no extra args); --build-dir overrides that discovery, and with neither
// the editor falls back to its own directory (the relocatable ship layout). --mcp[=port] runs an
// MCP server exposing the editor to an agent (read-only by default; --mcp-write enables mutations).
int main(const int argc, char** argv)
{
    const Veng::vector<Veng::string> args(argv, argv + argc);

    // A no-op query flag that prints the build version and exits before any window or device is
    // created — the source-tree-free identity probe the SDK conformance test runs to cover the
    // installed exe's runtime resolution without a project.
    for (Veng::usize i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--version")
        {
            std::printf("veng-editor %s\n", VENG_EDITOR_VERSION);
            return 0;
        }
    }

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

    const McpOptions mcpOptions = ParseMcpOptions(args);

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
        // The cooker's id-generator, run in-process against the project's reference packs.
        .MintId = [](std::span<const Veng::path> references) -> Veng::AssetId
        {
            const Veng::Result<Veng::AssetId> id = Veng::Cook::GenerateAssetId(references);
            if (!id)
            {
                Veng::Log::Error("editor: AssetId mint failed: {}", id.error());
                return Veng::AssetId{};
            }
            return *id;
        },
    };

    // The editor consumes its own --project/--build-dir flags above; Application::Run reads only a
    // launcher-convention working-directory arg, so hand it just the program name (the editor
    // resolves the project and build dir as absolute paths, needing no working-directory selector).
    Veng::Unique<VengEditor::EditorHost> host = VengEditor::EditorHost::Create(info);

    // The MCP server is constructed only under --mcp, so the editor's default launch and its smokes
    // are unaffected; it drives Pump() through the host's frame pump and stops with its Unique.
    // Construction is deferred to the host's post-initialize callback: the service's McpHost binds
    // GetAssetManager() by reference, which exists only once Run() has initialized the engine.
    Veng::Unique<EditorMcpService> mcp;
    if (mcpOptions.Enabled)
    {
        host->SetOnInitialized([&mcp, &host, mcpOptions] { mcp = StartMcp(*host, mcpOptions); });
    }

    host->Run({args.front()});

    return 0;
}
