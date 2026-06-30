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

#include <VengGraph/MaterialCatalog.h>

#include "AssetEditorPanel.h"
#include "AssetSourceIndex.h"
#include "CommandStack.h"
#include "PreviewCapability.h"
#include "StatusTracker.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/InspectorPanel.h"
#include "panels/LevelEditorPanel.h"
#include "panels/MaterialEditorPanel.h"
#include "panels/MaterialInstanceEditorPanel.h"
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

        // Reads the engine core pack's source manifest from the same .veng/build.json sidecar. The
        // editor's cook-on-demand passes it as a reference so a recooked material resolves core-pack
        // ids (the standard vertex shaders), matching the --reference the file-based cook passes.
        // nullopt when no project, no sidecar, or no "corePackManifest" key.
        optional<path> DiscoverCorePackManifest(const optional<path>& projectPath)
        {
            if (!projectPath)
            {
                return std::nullopt;
            }
            const path sidecar = projectPath->parent_path() / ".veng" / "build.json";
            const optional<nlohmann::json> doc = ReadJsonObject(sidecar);
            if (!doc || !doc->contains("corePackManifest") ||
                !(*doc)["corePackManifest"].is_string())
            {
                return std::nullopt;
            }
            return path{(*doc)["corePackManifest"].get<string>()};
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
                                  VengEditor::CookDriver cook, function<AssetId()> mintId)
                : m_Index(index), m_App(app), m_Assets(assets), m_ImGui(imgui), m_Editors(editors),
                  m_Cook(std::move(cook)), m_MintId(std::move(mintId))
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
                                                         m_Assets, m_ImGui, m_Editors, m_Cook,
                                                         m_MintId);
            }

        private:
            const AssetSourceIndex& m_Index;
            Application& m_App;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            EditorRegistry& m_Editors;
            VengEditor::CookDriver m_Cook;
            function<AssetId()> m_MintId;
        };

        // Resolves a material-instance AssetId to its .vmatinst.json source through the manifest
        // index, then opens a MaterialInstanceEditorPanel wired to the host's engine refs.
        class MaterialInstanceEditorFactory final : public AssetEditorFactory
        {
        public:
            MaterialInstanceEditorFactory(const AssetSourceIndex& index, Application& app,
                                          AssetManager& assets, ImGuiLayer& imgui,
                                          VengEditor::CookDriver cook)
                : m_Index(index), m_App(app), m_Assets(assets), m_ImGui(imgui),
                  m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error(
                        "Material-instance editor: no source manifest entry for asset 0x{:X}",
                        id.Value);
                    return nullptr;
                }

                return CreateUnique<MaterialInstanceEditorPanel>(id, entry->Source, m_Index, m_App,
                                                                 m_Assets, m_ImGui, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Application& m_App;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
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

        // The material node editor's reflection inspector resolves a node's enum property
        // (Param provenance, Constant leaf type) through this registry; register those graph
        // enum types beside the builtins so the lookup never misses.
        VengGraph::RegisterMaterialGraphTypes(registries->Types);

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

        // The engine core pack manifest the cook-on-demand references; empty in the relocatable ship
        // layout (no sidecar), where the editor edits no source and never recooks.
        const path corePackManifest = DiscoverCorePackManifest(info.ProjectPath).value_or(path{});

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
        return Unique<EditorHost>(new EditorHost(
            info, std::move(settings), std::move(buildDir), std::move(corePackManifest),
            std::move(registries), std::move(gameModulePtr), std::move(editorModule)));
    }

    EditorHost::EditorHost(const EditorHostInfo& info, ProjectSettings settings, path buildDir,
                           path corePackManifest, Unique<Registries> registries,
                           Unique<LoadedModule> gameModule, optional<LoadedModule> editorModule)
        : Application(info.App, registries->Types, registries->Systems), m_Info(info),
          m_Registries(std::move(registries)), m_GameModule(std::move(gameModule)),
          m_EditorModule(std::move(editorModule)), m_ProjectSettings(std::move(settings)),
          m_ProjectFile(info.ProjectPath.value_or(path{})), m_BuildDir(std::move(buildDir)),
          m_CorePackManifest(std::move(corePackManifest))
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

        m_Status = CreateUnique<StatusTracker>();

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
                AssetType::Material,
                CreateUnique<MaterialEditorFactory>(*m_Sources, *this, GetAssetManager(),
                                                    *GetImGuiLayer(), m_Registries->Editor,
                                                    cookFor(), [this] { return MintAssetId(); }));

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::MaterialInstance,
                CreateUnique<MaterialInstanceEditorFactory>(*m_Sources, *this, GetAssetManager(),
                                                            *GetImGuiLayer(), cookFor()));

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

    AssetId EditorHost::MintAssetId() const
    {
        if (!m_Info.MintId)
        {
            return AssetId{};
        }

        // The same reference set RequestCook resolves against: every project pack plus the core
        // pack, so the minted id avoids the whole project's one AssetId namespace.
        vector<path> references = m_ProjectSettings.Packs;
        if (!m_CorePackManifest.empty())
        {
            references.push_back(m_CorePackManifest);
        }
        return m_Info.MintId(references);
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

        // Inject every project pack manifest plus the engine core pack so the cook resolves
        // cross-asset references by AssetId across the whole project (one namespace) and against the
        // core pack's built-in assets (the standard vertex shaders); panels build manifest-agnostic
        // requests. This mirrors the --reference set the file-based add_project cook passes.
        CookRequest resolved = request;
        resolved.ReferenceManifests = m_ProjectSettings.Packs;
        if (!m_CorePackManifest.empty())
        {
            resolved.ReferenceManifests.push_back(m_CorePackManifest);
            // The engine core shaders sit in the core pack's own directory; threading it as the
            // shader-include dir lets a recooked material's fragment resolve `#include
            // "Veng/material.slang"`, mirroring the file-based cook's --shader-include.
            resolved.ShaderIncludeDir = m_CorePackManifest.parent_path() / "shaders";
        }

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

        // Track the cook for the status bar; the continuation ends it (both success and
        // failure run through Then). m_Status is non-null once OnInitialize has run, which
        // precedes any cook.
        const StatusTracker::TaskId statusTask =
            m_Status->Begin(fmt::format("Cooking {}", request.SourcePath.filename().string()));

        Task<vector<u8>> task = m_Info.Cook(resolved, GetTaskSystem());

        task.Then(
            [this, statusTask, targetId = request.TargetId, source = request.SourcePath,
             onComplete = std::move(onComplete)](Result<vector<u8>> bytes) mutable
            {
                m_Status->End(statusTask);

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

    AssetEditorPanel* EditorHost::FocusedAssetEditor()
    {
        for (PanelSlot& slot : m_Panels)
        {
            auto* editor = dynamic_cast<AssetEditorPanel*>(slot.Panel.get());
            if (editor != nullptr && editor->IsDocumentFocused())
            {
                return editor;
            }
        }
        return nullptr;
    }

    CommandStack* EditorHost::FocusedCommandStack()
    {
        AssetEditorPanel* editor = FocusedAssetEditor();
        return editor != nullptr ? editor->GetCommandStack() : nullptr;
    }

    void EditorHost::DrawMenuBar()
    {
        if (auto bar = UI::MainMenuBar())
        {
            if (auto file = UI::Menu("File"))
            {
                // Save targets the focused document; enabled only when one is focused and has a
                // dirty stack, so the action reflects whether there is anything to write.
                AssetEditorPanel* editor = FocusedAssetEditor();
                CommandStack* stack = editor != nullptr ? editor->GetCommandStack() : nullptr;
                const bool canSave = stack != nullptr && stack->IsDirty();
                if (UI::MenuItem("Save", canSave))
                {
                    const VoidResult saved = editor->Save();
                    if (!saved)
                    {
                        Log::Error("editor: save failed: {}", saved.error());
                    }
                }

                if (UI::MenuItem("Exit"))
                {
                    RequestExit();
                }
            }

            if (auto edit = UI::Menu("Edit"))
            {
                // The Edit menu targets whichever document is focused; with none focused (or one
                // without a stack) the items are disabled. The labels carry the next edit's title.
                CommandStack* stack = FocusedCommandStack();
                const bool canUndo = stack != nullptr && stack->CanUndo();
                const bool canRedo = stack != nullptr && stack->CanRedo();
                const string undoLabel =
                    canUndo ? fmt::format("Undo {}", stack->UndoTitle()) : string{"Undo"};
                const string redoLabel =
                    canRedo ? fmt::format("Redo {}", stack->RedoTitle()) : string{"Redo"};
                if (UI::MenuItem(undoLabel, canUndo))
                {
                    stack->Undo();
                }
                if (UI::MenuItem(redoLabel, canRedo))
                {
                    stack->Redo();
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

    void EditorHost::DrawStatusBar()
    {
        // BeginViewportSideBar reserves the strip from the viewport work area (the same
        // next-frame inset mechanism the main menu bar uses), so the dockspace fits above it.
        // The bar window is always submitted to keep that reservation stable frame to frame.
        // Size the strip to fit a full-height progress bar inside the window padding (unlike
        // the menu bar, this content window has WindowPadding).
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 height = ImGui::GetFrameHeight() + (style.WindowPadding.y * 2.0f);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
        const bool open = ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(),
                                                      ImGuiDir_Down, height, flags);
        if (open)
        {
            const StatusTracker::Snapshot status = m_Status->GetSnapshot();
            const usize count = status.Tasks.size();

            // The collapsed summary: idle shows an empty bar, a single task its own label with
            // an indeterminate sweep (a cook reports no sub-step progress), and several the
            // count with the wave's completed/total fill.
            string label;
            f32 fraction = 0.0f;
            if (count == 0)
            {
                label = "No tasks running";
            }
            else if (count == 1)
            {
                label = status.Tasks.front();
                fraction = -1.0f;
            }
            else
            {
                label = fmt::format("{} tasks running", count);
                fraction = status.TotalInWave > 0 ? static_cast<f32>(status.CompletedInWave) /
                                                        static_cast<f32>(status.TotalInWave)
                                                  : 0.0f;
            }

            // Right-align a small label + bar, taking only as much width as they need.
            const f32 barWidth = 140.0f;
            const f32 labelWidth = ImGui::CalcTextSize(label.c_str()).x;
            const f32 groupWidth = labelWidth + style.ItemInnerSpacing.x + barWidth;
            const f32 avail = ImGui::GetContentRegionAvail().x;
            if (avail > groupWidth)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - groupWidth));
            }

            ImGui::BeginGroup();
            UI::AlignTextToFramePadding();
            UI::Text(label);
            UI::SameLine();
            UI::ProgressBar(fraction, vec2{barWidth, 0.0f});
            ImGui::EndGroup();

            // Clicking the summary expands a popup listing every running task with its own bar.
            // Only meaningful while tasks run; idle has nothing to expand.
            if (count > 0)
            {
                const bool hovered =
                    ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (hovered)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        UI::OpenPopup("##StatusTaskList");
                    }
                }
            }

            if (auto popup = UI::Popup("##StatusTaskList"))
            {
                UI::SeparatorText("Running tasks");
                for (const string& task : status.Tasks)
                {
                    UI::AlignTextToFramePadding();
                    UI::Text(task);
                    UI::SameLine();
                    UI::ProgressBar(-1.0f, vec2{160.0f, 0.0f});
                }
            }
        }
        ImGui::End();
    }

    u32 EditorHost::BuildDefaultHostLayout(u32 dockspaceId)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        ImGuiID center = dockspaceId;
        const ImGuiID bottom =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

        // Flag the upper region as the central node so an asset editor docks into the main dock
        // area by id. The flag is saved to imgui.ini, so it survives a restart and stays
        // discoverable from a restored layout.
        if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(center))
        {
            centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
        }

        // The asset browser, console and project settings share the bottom strip as tabs; any
        // other host panel falls through to the central node. Each panel's top-level window name
        // is its GetTitle(), so the three bottom panels dock by name. Asset editors are not in
        // m_Panels yet — they dock into the central node through the pending-adoption path.
        for (const PanelSlot& slot : m_Panels)
        {
            const string title{slot.Panel->GetTitle()};
            const bool docksBottom =
                title == "Asset Browser" || title == "Console" || title == "Project Settings";
            ImGui::DockBuilderDockWindow(title.c_str(), docksBottom ? bottom : center);
        }

        ImGui::DockBuilderFinish(dockspaceId);
        return center;
    }

    void EditorHost::OnRender()
    {
        const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport();

        // First frame with no restored imgui.ini layout: lay out the default docking. An empty
        // node means no layout was loaded; a restored one is non-empty and left untouched so the
        // user's docking survives a restart. BuildDefaultHostLayout returns the central node id —
        // its CentralNode pointer is not refreshed until the next dockspace update, so the query
        // below is unreliable on the build frame and the returned id is used instead.
        ImGuiID centerNode = 0;
        if (!m_HostLayoutBuilt)
        {
            m_HostLayoutBuilt = true;
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
            if (node == nullptr || node->IsEmpty())
            {
                centerNode = BuildDefaultHostLayout(dockspaceId);
            }
        }

        // On any frame past the build, resolve the live central node. It is saved to imgui.ini,
        // so a restored layout finds it too; a layout predating the central-node flag leaves the
        // query null, in which case a new editor window falls back to floating.
        if (centerNode == 0)
        {
            if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
            {
                centerNode = central->ID;
            }
        }

        // Adopt any panels opened via OpenAssetEditor since last frame, before drawing them so a
        // freshly opened editor draws this frame. A panel's Offscreen viewport is registered in
        // its constructor, so it joins the drive-list for the next frame. Every asset editor —
        // the startup document and any opened from the asset browser alike — docks into the
        // central node, so it lands in the main dock area rather than floating.
        for (Unique<EditorPanel>& opened : m_PendingPanels)
        {
            if (centerNode != 0)
            {
                const string title{opened->GetTitle()};
                ImGui::DockBuilderDockWindow(title.c_str(), centerNode);
            }
            m_Panels.push_back({.Panel = std::move(opened), .Open = true, .Document = true});
        }
        m_PendingPanels.clear();

        DrawMenuBar();
        DrawStatusBar();

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

        // Undo/redo on the focused document: Ctrl/Cmd+Z undo, Ctrl/Cmd+Shift+Z redo. The panels
        // drew above, so the focus flags are current. Suppressed while a text widget is capturing
        // keys, so typing a 'z' into a field never undoes; resolved after Draw so the shortcut
        // targets the document focused this frame.
        if (!ImGui::GetIO().WantTextInput && (UI::IsCtrlDown() || UI::IsSuperDown()) &&
            UI::IsKeyPressed(UI::Key::Z))
        {
            if (CommandStack* stack = FocusedCommandStack())
            {
                if (UI::IsShiftDown())
                {
                    stack->Redo();
                }
                else
                {
                    stack->Undo();
                }
            }
        }

        // Ctrl/Cmd+S writes the focused document's source. Same WantTextInput suppression and
        // post-Draw focus resolution as undo/redo, dispatched to the focused document's Save.
        if (!ImGui::GetIO().WantTextInput && (UI::IsCtrlDown() || UI::IsSuperDown()) &&
            UI::IsKeyPressed(UI::Key::S))
        {
            if (AssetEditorPanel* editor = FocusedAssetEditor())
            {
                const VoidResult saved = editor->Save();
                if (!saved)
                {
                    Log::Error("editor: save failed: {}", saved.error());
                }
            }
        }

        // Destroy any document whose window was closed this frame. Closing the ✕ clears the slot's
        // Open flag; erasing the slot runs the panel's destructor, which drops its Offscreen
        // viewport(s) — self-unregistering from the engine drive-list so a closed editor stops
        // rendering — and removes its dock windows. Tool panels (Document == false) only hide, so
        // the Window menu can reopen them. Done after Draw so the panel that handled the close this
        // frame is torn down between frames, not mid-iteration.
        std::erase_if(m_Panels, [](const PanelSlot& slot) { return slot.Document && !slot.Open; });
    }

    void EditorHost::OnDispose()
    {
        // Panels drop their owned Offscreen viewports here (each self-unregisters from the
        // base drive-list) before the base tears the context down.
        m_Panels.clear();
        m_PendingPanels.clear();
        m_Sources.reset();
        m_Status.reset();
    }
}
