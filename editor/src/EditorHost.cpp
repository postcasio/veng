#include <VengEditor/EditorHost.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include "AssetSourceIndex.h"
#include "PreviewCapability.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/InspectorPanel.h"
#include "panels/LevelEditorPanel.h"
#include "panels/MaterialEditorPanel.h"
#include "panels/PrefabEditorPanel.h"
#include "panels/ProjectSettingsPanel.h"
#include "panels/TextureEditorPanel.h"

#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Writes the format a role resolves to into the fixed RoleToFormat record.
        void SetRoleFormat(RoleToFormat& table, CompressionRole role, CompressionFormat format)
        {
            switch (role)
            {
            case CompressionRole::Color:
                table.Color = format;
                return;
            case CompressionRole::Normal:
                table.Normal = format;
                return;
            case CompressionRole::Mask:
                table.Mask = format;
                return;
            case CompressionRole::HDR:
                table.HDR = format;
                return;
            case CompressionRole::UI:
                table.UI = format;
                return;
            }
        }

        // Reads a JSON file into a parsed object, or nullopt on missing/malformed input.
        optional<nlohmann::json> ReadJsonObject(const path& file)
        {
            const std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return std::nullopt;
            }
            std::ostringstream contents;
            contents << stream.rdbuf();
            nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                return std::nullopt;
            }
            return parsed;
        }

        // Parses one *.buildcfg authoring file into a BuildConfiguration, the same schema the
        // ProjectSettingsPanel writes and the cooker reads. Enums parse by name; an absent field
        // keeps its default.
        optional<BuildConfiguration> ParseBuildConfigFile(const path& file)
        {
            const optional<nlohmann::json> cfg = ReadJsonObject(file);
            if (!cfg)
            {
                return std::nullopt;
            }

            BuildConfiguration config;
            if (cfg->contains("name") && (*cfg)["name"].is_string())
            {
                config.Name = (*cfg)["name"].get<string>();
            }
            if (cfg->contains("target") && (*cfg)["target"].is_string())
            {
                config.Target = (*cfg)["target"].get<string>();
            }
            if (cfg->contains("outputSuffix") && (*cfg)["outputSuffix"].is_string())
            {
                config.OutputSuffix = (*cfg)["outputSuffix"].get<string>();
            }
            if (cfg->contains("compressionLevel") && (*cfg)["compressionLevel"].is_number_integer())
            {
                config.CompressionLevel = (*cfg)["compressionLevel"].get<i32>();
            }
            if (cfg->contains("formats") && (*cfg)["formats"].is_object())
            {
                const nlohmann::json& formats = (*cfg)["formats"];
                for (const CompressionRole role : CompressionRoles)
                {
                    const string roleName{ToString(role)};
                    if (!formats.contains(roleName) || !formats[roleName].is_string())
                    {
                        continue;
                    }
                    if (const optional<CompressionFormat> format =
                            ParseCompressionFormat(formats[roleName].get<string>()))
                    {
                        SetRoleFormat(config.Formats, role, *format);
                    }
                }
            }
            return config;
        }

        // Reads the build-output dir the build recorded beside the project, in its .veng/build.json
        // sidecar (the dir holding the cooked packs + module libraries). A gitignored, machine-local
        // file written by veng_add_editor at configure time. nullopt when no project, no sidecar, or
        // no "buildDir" key — the caller then falls back to an override or the editor's own directory.
        optional<path> DiscoverProjectBuildDir(const optional<path>& projectPath)
        {
            if (!projectPath)
            {
                return std::nullopt;
            }
            const path sidecar = projectPath->parent_path() / ".veng" / "build.json";
            const optional<nlohmann::json> doc = ReadJsonObject(sidecar);
            if (!doc || !doc->contains("buildDir") || !(*doc)["buildDir"].is_string())
            {
                return std::nullopt;
            }
            return path{(*doc)["buildDir"].get<string>()};
        }

        // Composes a module's shared-library file name from its logical name, per platform — the
        // editor resolves the project-named module beside the build output (lib<name>.<ext>).
        path ModuleFileName(const string& name)
        {
#if defined(_WIN32)
            return path{name + ".dll"};
#elif defined(__APPLE__)
            return path{"lib" + name + ".dylib"};
#else
            return path{"lib" + name + ".so"};
#endif
        }

        // Loads project.veng (a list of *.buildcfg file names + the active configuration name)
        // into a ProjectSettings. A missing or malformed file yields the empty zero-config state.
        ProjectSettings LoadProjectSettings(const path& projectFile)
        {
            ProjectSettings settings;
            const optional<nlohmann::json> project = ReadJsonObject(projectFile);
            if (!project)
            {
                return settings;
            }

            if (project->contains("activeConfiguration") &&
                (*project)["activeConfiguration"].is_string())
            {
                settings.ActiveConfiguration = (*project)["activeConfiguration"].get<string>();
            }

            if (project->contains("startupLevel") &&
                (*project)["startupLevel"].is_number_unsigned())
            {
                settings.StartupLevel = AssetId{.Value = (*project)["startupLevel"].get<u64>()};
            }

            // The module(s) the editor dlopens, named logically (the build writes lib<name>.<ext>
            // beside the build output, where the editor resolves them).
            if (project->contains("module") && (*project)["module"].is_string())
            {
                settings.Module = (*project)["module"].get<string>();
            }
            if (project->contains("editorModule") && (*project)["editorModule"].is_string())
            {
                settings.EditorModule = (*project)["editorModule"].get<string>();
            }

            const path dir = projectFile.parent_path();

            // The packs the project owns, resolved to absolute source-manifest paths.
            if (project->contains("packs") && (*project)["packs"].is_array())
            {
                for (const nlohmann::json& entry : (*project)["packs"])
                {
                    if (entry.is_string())
                    {
                        settings.Packs.push_back(dir / entry.get<string>());
                    }
                }
            }

            if (project->contains("configurations") && (*project)["configurations"].is_array())
            {
                for (const nlohmann::json& entry : (*project)["configurations"])
                {
                    if (!entry.is_string())
                    {
                        continue;
                    }
                    if (optional<BuildConfiguration> config =
                            ParseBuildConfigFile(dir / entry.get<string>()))
                    {
                        settings.Configurations.push_back(std::move(*config));
                    }
                }
            }
            return settings;
        }

        // Resolves a texture AssetId to its .tex.json source through the manifest
        // index, then opens a TextureEditorPanel wired to the host's engine refs.
        class TextureEditorFactory final : public AssetEditorFactory
        {
        public:
            TextureEditorFactory(const AssetSourceIndex& index, Renderer::Context& context,
                                 AssetManager& assets, ImGuiLayer& imgui,
                                 VengEditor::CookDriver cook, ActiveConfigAccessor activeConfig,
                                 PreviewConfigAccessor previewConfig)
                : m_Index(index), m_Context(context), m_Assets(assets), m_ImGui(imgui),
                  m_Cook(std::move(cook)), m_ActiveConfig(std::move(activeConfig)),
                  m_PreviewConfig(std::move(previewConfig))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Texture editor: no source manifest entry for asset 0x{:X}",
                               id.Value);
                    return nullptr;
                }

                return CreateUnique<TextureEditorPanel>(id, entry->Source, m_Context, m_Assets,
                                                        m_ImGui, m_Cook, m_ActiveConfig,
                                                        m_PreviewConfig);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            VengEditor::CookDriver m_Cook;
            ActiveConfigAccessor m_ActiveConfig;
            PreviewConfigAccessor m_PreviewConfig;
        };

        // Resolves a material AssetId to its .vmat.json source through the manifest
        // index, then opens a MaterialEditorPanel wired to the host's engine refs.
        class MaterialEditorFactory final : public AssetEditorFactory
        {
        public:
            MaterialEditorFactory(const AssetSourceIndex& index, Application& app,
                                  AssetManager& assets, ImGuiLayer& imgui, EditorRegistry& editors,
                                  VengEditor::CookDriver cook)
                : m_Index(index), m_App(app), m_Assets(assets), m_ImGui(imgui), m_Editors(editors),
                  m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Material editor: no source manifest entry for asset 0x{:X}",
                               id.Value);
                    return nullptr;
                }

                return CreateUnique<MaterialEditorPanel>(id, entry->Source, m_Index, m_App,
                                                         m_Assets, m_ImGui, m_Editors, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Application& m_App;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            EditorRegistry& m_Editors;
            VengEditor::CookDriver m_Cook;
        };

        // Opens a PrefabEditorPanel that spawns the prefab into a live Scene for editing.
        // Needs no manifest source: the prefab is edited in-scene, not recooked.
        class PrefabEditorFactory final : public AssetEditorFactory
        {
        public:
            PrefabEditorFactory(Application& app, AssetManager& assets, ImGuiLayer& imgui,
                                TypeRegistry& types, EditorRegistry& editors,
                                const AssetSourceIndex& sources, Input& input, InputRouter& router,
                                SystemRegistry& systems)
                : m_App(app), m_Assets(assets), m_ImGui(imgui), m_Types(types), m_Editors(editors),
                  m_Sources(sources), m_Input(input), m_Router(router), m_Systems(systems)
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                return CreateUnique<PrefabEditorPanel>(id, m_App, m_Assets, m_ImGui, m_Types,
                                                       m_Editors, m_Sources, m_Input, m_Router,
                                                       m_Systems);
            }

        private:
            Application& m_App;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            TypeRegistry& m_Types;
            EditorRegistry& m_Editors;
            const AssetSourceIndex& m_Sources;
            Input& m_Input;
            InputRouter& m_Router;
            SystemRegistry& m_Systems;
        };

        // Resolves a level AssetId to its world prefab (by loading the level) and its
        // .level.json source (through the manifest index), then opens a LevelEditorPanel.
        class LevelEditorFactory final : public AssetEditorFactory
        {
        public:
            LevelEditorFactory(const AssetSourceIndex& index, Application& app,
                               AssetManager& assets, ImGuiLayer& imgui, TypeRegistry& types,
                               EditorRegistry& editors, Input& input, InputRouter& router,
                               SystemRegistry& systems, VengEditor::CookDriver cook)
                : m_Index(index), m_App(app), m_Assets(assets), m_ImGui(imgui), m_Types(types),
                  m_Editors(editors), m_Input(input), m_Router(router), m_Systems(systems),
                  m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Level editor: no source manifest entry for asset 0x{:X}", id.Value);
                    return nullptr;
                }

                // The level names its world prefab by id; load it to resolve the prefab the
                // scene surface opens for layout editing.
                const AssetResult<AssetHandle<Level>> level = m_Assets.LoadSync<Level>(id);
                if (!level.has_value())
                {
                    Log::Error("Level editor: failed to load level 0x{:X}: {}", id.Value,
                               level.error().Detail);
                    return nullptr;
                }
                const AssetId worldPrefab = level->Get()->GetWorld().Id();

                return CreateUnique<LevelEditorPanel>(
                    id, worldPrefab, entry->Source, m_App, m_Assets, m_ImGui, m_Types, m_Editors,
                    m_Index, m_Input, m_Router, m_Systems, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Application& m_App;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            TypeRegistry& m_Types;
            EditorRegistry& m_Editors;
            Input& m_Input;
            InputRouter& m_Router;
            SystemRegistry& m_Systems;
            VengEditor::CookDriver m_Cook;
        };
    }

    // Owns the host-side registries so the base Application can borrow the TypeRegistry.
    struct EditorHost::Registries
    {
        ApplicationRegistry App;
        TypeRegistry Types;
        SystemRegistry Systems;
        EditorRegistry Editor;
    };

    Unique<EditorHost> EditorHost::Create(const EditorHostInfo& info)
    {
        auto registries = CreateUnique<Registries>();

        RegisterBuiltinTypes(registries->Types);
        RegisterBuiltinSystems(registries->Systems);

        // The project is the editor's entrypoint: it names the module(s) to load, resolved as
        // shared libraries beside the project's build output.
        ProjectSettings settings;
        if (info.ProjectPath)
        {
            settings = LoadProjectSettings(*info.ProjectPath);
        }

        // Resolve the build-output dir (cooked packs + module libraries): an explicit --build-dir
        // override wins; else the dir the build recorded beside the project (.veng/build.json), so
        // launching with only a project self-discovers it; else the editor's own directory (the
        // relocatable ship layout, where packs + module sit beside the editor).
        path buildDir;
        if (info.BuildDir)
        {
            buildDir = *info.BuildDir;
        }
        else if (const optional<path> discovered = DiscoverProjectBuildDir(info.ProjectPath))
        {
            buildDir = *discovered;
        }
        else
        {
            buildDir = ExecutableDirectory();
        }

        VE_ASSERT(!settings.Module.empty(),
                  "editor: project names no module (its \"module\" key); nothing to load");
        const path gameModulePath = buildDir / ModuleFileName(settings.Module);
        Result<LoadedModule> gameModule = ModuleLoader::Load(gameModulePath);
        VE_ASSERT(gameModule.has_value(), "editor: failed to load game module {}: {}",
                  gameModulePath.string(), gameModule.error());

        optional<LoadedModule> editorModule;
        if (!settings.EditorModule.empty())
        {
            const path editorModulePath = buildDir / ModuleFileName(settings.EditorModule);
            Result<LoadedModule> loaded = ModuleLoader::Load(editorModulePath);
            VE_ASSERT(loaded.has_value(), "editor: failed to load editor module {}: {}",
                      editorModulePath.string(), loaded.error());
            editorModule.emplace(std::move(*loaded));
        }

        VengModuleHost host{
            .App = registries->App,
            .Types = registries->Types,
            .Systems = registries->Systems,
            .Editor = &registries->Editor,
        };
        gameModule->Register(host);
        if (editorModule)
        {
            editorModule->Register(host);
        }

        auto gameModulePtr = CreateUnique<LoadedModule>(std::move(*gameModule));
        return Unique<EditorHost>(new EditorHost(info, std::move(settings), std::move(buildDir),
                                                 std::move(registries), std::move(gameModulePtr),
                                                 std::move(editorModule)));
    }

    EditorHost::EditorHost(const EditorHostInfo& info, ProjectSettings settings, path buildDir,
                           Unique<Registries> registries, Unique<LoadedModule> gameModule,
                           optional<LoadedModule> editorModule)
        : Application(info.App, registries->Types, registries->Systems), m_Info(info),
          m_Registries(std::move(registries)), m_GameModule(std::move(gameModule)),
          m_EditorModule(std::move(editorModule)), m_ProjectSettings(std::move(settings)),
          m_ProjectFile(info.ProjectPath.value_or(path{})), m_BuildDir(std::move(buildDir))
    {
    }

    EditorHost::~EditorHost() = default;

    void EditorHost::OpenAssetEditor(AssetType type, AssetId id)
    {
        if (Unique<EditorPanel> panel = m_Registries->Editor.CreateEditorFor(type, id))
        {
            m_PendingPanels.push_back(std::move(panel));
        }
    }

    bool EditorHost::HasAssetEditor(AssetType type) const
    {
        return m_Registries->Editor.AssetEditorFor(type) != nullptr;
    }

    const BuildConfiguration* EditorHost::GetActiveConfiguration() const
    {
        for (const BuildConfiguration& config : m_ProjectSettings.Configurations)
        {
            if (config.Name == m_ProjectSettings.ActiveConfiguration)
            {
                return &config;
            }
        }
        return nullptr;
    }

    BuildConfiguration EditorHost::GetPreviewConfiguration()
    {
        // "Preview as ship config" is honored only while the named configuration stays
        // previewable on this GPU; otherwise (and by default) the editor previews host-safe so
        // it can never hand the device a blob it cannot sample.
        if (m_PreviewShipConfig)
        {
            for (const BuildConfiguration& config : m_ProjectSettings.Configurations)
            {
                if (config.Name == *m_PreviewShipConfig &&
                    IsConfigPreviewable(config, GetRenderContext()).Previewable)
                {
                    return config;
                }
            }
        }
        return HostSafeConfiguration();
    }

    const optional<string>& EditorHost::GetPreviewShipConfig() const
    {
        return m_PreviewShipConfig;
    }

    void EditorHost::SetPreviewShipConfig(optional<string> name)
    {
        m_PreviewShipConfig = std::move(name);
    }

    void EditorHost::OnInitialize()
    {
        VE_ASSERT(GetImGuiLayer() != nullptr, "editor host requires the ImGui layer");

        // The project settings (configurations, packs, module names, startup level) were loaded in
        // Create, which also resolved the build-output dir (override / sidecar discovery / the
        // editor's own directory). The cooked packs live there alongside the module libraries.
        const path& buildDir = m_BuildDir;

        // Mount each cooked pack the project names. The cooked pack sits in the build dir (copied by
        // veng_add_game) under the source manifest's stem: assets/sample.vengpack.json ->
        // sample.vengpack.
        for (const path& packSource : m_ProjectSettings.Packs)
        {
            const path mountName = packSource.stem();
            const VoidResult mount = GetAssetManager().Mount(buildDir / mountName);
            VE_ASSERT(mount, "{}", mount.error());
        }

        // The editor's own icon pack (light/camera billboard textures) sits beside the exe.
        // The engine ships no icon content; the viewport gizmos resolve their TextureHandles
        // from these. Mounting is best-effort — a missing pack only drops the gizmo icons.
        const VoidResult iconMount =
            GetAssetManager().Mount(ExecutableDirectory() / "editor_icons.vengpack");
        if (!iconMount)
        {
            Log::Warn("editor: icon pack not mounted: {}", iconMount.error());
        }

        // Built from the union of the project's pack manifests; an empty index when no project is
        // configured keeps the picker candidate-free rather than absent.
        m_Sources =
            CreateUnique<AssetSourceIndex>(AssetSourceIndex::ParsePacks(m_ProjectSettings.Packs));

        // The project-settings panel inspects ProjectSettings through reflection; registering
        // it auto-registers its compression enums, whose VE_ENUM tables drive the named combos.
        GetTypeRegistry().Register<ProjectSettings>();

        // A prefab is edited live in a spawned Scene, so its editor needs no manifest
        // source; register it unconditionally.
        m_Registries->Editor.RegisterAssetEditor(
            AssetType::Prefab,
            CreateUnique<PrefabEditorFactory>(*this, GetAssetManager(), *GetImGuiLayer(),
                                              GetTypeRegistry(), m_Registries->Editor, *m_Sources,
                                              GetInput(), GetInputRouter(), GetSystemRegistry()));

        // try_emplace no-ops if the game module already registered a factory for these types.
        if (m_Info.ProjectPath)
        {
            auto cookFor = [this]
            {
                return VengEditor::CookDriver([this](const VengEditor::CookRequest& request,
                                                     function<void(Result<MountHandle>)> onComplete)
                                              { RequestCook(request, std::move(onComplete)); });
            };

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Texture,
                CreateUnique<TextureEditorFactory>(
                    *m_Sources, GetRenderContext(), GetAssetManager(), *GetImGuiLayer(), cookFor(),
                    [this] { return GetActiveConfiguration(); },
                    [this] { return GetPreviewConfiguration(); }));

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Material, CreateUnique<MaterialEditorFactory>(
                                         *m_Sources, *this, GetAssetManager(), *GetImGuiLayer(),
                                         m_Registries->Editor, cookFor()));

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Level, CreateUnique<LevelEditorFactory>(
                                      *m_Sources, *this, GetAssetManager(), *GetImGuiLayer(),
                                      GetTypeRegistry(), m_Registries->Editor, GetInput(),
                                      GetInputRouter(), GetSystemRegistry(), cookFor()));
        }

        // The asset browser reads the first mounted pack (cooked in the build dir under the source
        // manifest's stem); an unconfigured project leaves it with an empty path.
        const path browserPack = m_ProjectSettings.Packs.empty()
                                     ? path{}
                                     : buildDir / m_ProjectSettings.Packs.front().stem();
        m_Panels.push_back({CreateUnique<AssetBrowserPanel>(browserPack, *m_Sources, *this), true});
        m_Panels.push_back({CreateUnique<ConsolePanel>(), true});
        m_Panels.push_back(
            {CreateUnique<ProjectSettingsPanel>(
                 m_ProjectSettings, m_ProjectFile, GetAssetManager(), m_Registries->Editor,
                 *m_Sources, GetRenderContext(),
                 [this]() -> const optional<string>& { return GetPreviewShipConfig(); },
                 [this](optional<string> name) { SetPreviewShipConfig(std::move(name)); }),
             true});

        for (Unique<EditorPanel>& panel : m_Registries->Editor.Panels())
        {
            m_Panels.push_back({std::move(panel), true});
        }

        // Open the project's startup level as the initial document so the editor starts on
        // live content; double-clicking any asset in the asset browser opens another. A
        // project with no startup level opens nothing.
        if (m_ProjectSettings.StartupLevel.IsValid())
        {
            OpenAssetEditor(AssetType::Level, m_ProjectSettings.StartupLevel);
        }
    }

    void EditorHost::RequestCook(const CookRequest& request,
                                 function<void(Result<MountHandle>)> onComplete)
    {
        if (!m_Info.Cook)
        {
            const string error = "editor: cook-on-demand backend not configured";
            Log::Error("{}", error);
            onComplete(std::unexpected(error));
            return;
        }

        // Inject every project pack manifest so the cook resolves cross-asset references by
        // AssetId across the whole project (one namespace); panels build manifest-agnostic requests.
        CookRequest resolved = request;
        resolved.ReferenceManifests = m_ProjectSettings.Packs;

        // A level cook validates its system ids and config against the module's reflected
        // catalogs, so inject the game module path (resolved beside the build output, as in
        // Create); non-level cooks ignore an empty value.
        if (!m_ProjectSettings.Module.empty())
        {
            resolved.ModulePath = m_BuildDir / ModuleFileName(m_ProjectSettings.Module);
        }

        // Thread the *preview* configuration, not the selected ship configuration: it is
        // host-safe by default (an uncompressed profile every GPU can sample), so the editor's
        // live preview can never cook a blob the device cannot sample. Opting into "preview as
        // ship config" substitutes a previewable ship configuration; the build of any
        // configuration stays unrestricted, only this editor cook is clamped.
        resolved.ActiveConfig = GetPreviewConfiguration();

        Task<vector<u8>> task = m_Info.Cook(resolved, GetTaskSystem());

        task.Then(
            [this, targetId = request.TargetId, source = request.SourcePath,
             onComplete = std::move(onComplete)](Result<vector<u8>> bytes) mutable
            {
                if (!bytes)
                {
                    Log::Error("editor: cook of '{}' failed: {}", source.string(), bytes.error());
                    onComplete(std::unexpected(bytes.error()));
                    return;
                }

                MountHandle handle = GetAssetManager().MountMemory(
                    std::move(*bytes), fmt::format("cook:{}", targetId.Value));
                onComplete(std::move(handle));
            });
    }

    void EditorHost::DrawMenuBar()
    {
        if (auto bar = UI::MainMenuBar())
        {
            if (auto file = UI::Menu("File"))
            {
                if (UI::MenuItem("Exit"))
                {
                    RequestExit();
                }
            }

            if (auto window = UI::Menu("Window"))
            {
                for (PanelSlot& slot : m_Panels)
                {
                    UI::MenuItem(slot.Panel->GetTitle(), &slot.Open);
                }
            }
        }
    }

    void EditorHost::BuildDefaultHostLayout(u32 dockspaceId)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        ImGuiID center = dockspaceId;
        const ImGuiID bottom =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

        // The asset browser, console and project settings share the bottom strip as tabs;
        // every other open window (the level-editor document) fills the space above. Every
        // panel's top-level window name is its GetTitle(), so the three bottom panels dock
        // by name and the document falls through to the center.
        for (const PanelSlot& slot : m_Panels)
        {
            const string title{slot.Panel->GetTitle()};
            const bool docksBottom =
                title == "Asset Browser" || title == "Console" || title == "Project Settings";
            ImGui::DockBuilderDockWindow(title.c_str(), docksBottom ? bottom : center);
        }

        ImGui::DockBuilderFinish(dockspaceId);
    }

    void EditorHost::OnRender()
    {
        // Adopt any panels opened via OpenAssetEditor since last frame, before drawing the
        // dockspace so a freshly opened editor draws this frame. A panel's Offscreen viewport
        // is registered in its constructor, so it joins the drive-list for the next frame.
        for (Unique<EditorPanel>& opened : m_PendingPanels)
        {
            m_Panels.push_back({std::move(opened), true});
        }
        m_PendingPanels.clear();

        const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport();

        // First frame with no restored imgui.ini layout: lay out the default docking.
        // An empty node means no layout was loaded; a restored one is non-empty and left
        // untouched so the user's docking survives a restart.
        if (!m_HostLayoutBuilt)
        {
            m_HostLayoutBuilt = true;
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
            if (node == nullptr || node->IsEmpty())
            {
                BuildDefaultHostLayout(dockspaceId);
            }
        }

        DrawMenuBar();

        // Each panel submits its own top-level window(s); an asset editor submits its
        // private dockspace and the children docked into it. The engine has already rendered
        // the panels' viewports, so a UI::Image samples a ready output. ImGuiLayer::Render and
        // the managed gather/composite (zero Presented placements) bracket this in the base.
        for (PanelSlot& slot : m_Panels)
        {
            if (slot.Open)
            {
                slot.Panel->Draw(&slot.Open);
            }
        }
    }

    void EditorHost::OnDispose()
    {
        // Panels drop their owned Offscreen viewports here (each self-unregisters from the
        // base drive-list) before the base tears the context down.
        m_Panels.clear();
        m_PendingPanels.clear();
        m_Sources.reset();
    }
}
