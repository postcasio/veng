#include <VengEditor/EditorHost.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>

#include "AssetSourceIndex.h"
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
        // The hello-triangle pack's sphere prefab, opened as the initial document.
        constexpr AssetId SampleScenePrefabId{0xA123F30FD219F2D5ULL};

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

            const path dir = projectFile.parent_path();
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
                                 VengEditor::CookDriver cook, ActiveConfigAccessor activeConfig)
                : m_Index(index), m_Context(context), m_Assets(assets), m_ImGui(imgui),
                  m_Cook(std::move(cook)), m_ActiveConfig(std::move(activeConfig))
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
                                                        m_ImGui, m_Cook, m_ActiveConfig);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            VengEditor::CookDriver m_Cook;
            ActiveConfigAccessor m_ActiveConfig;
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

        Result<LoadedModule> gameModule = ModuleLoader::Load(info.GameModulePath);
        VE_ASSERT(gameModule.has_value(), "editor: failed to load game module {}: {}",
                  info.GameModulePath.string(), gameModule.error());

        optional<LoadedModule> editorModule;
        if (info.EditorModulePath)
        {
            Result<LoadedModule> loaded = ModuleLoader::Load(*info.EditorModulePath);
            VE_ASSERT(loaded.has_value(), "editor: failed to load editor module {}: {}",
                      info.EditorModulePath->string(), loaded.error());
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
            info, std::move(registries), std::move(gameModulePtr), std::move(editorModule)));
    }

    EditorHost::EditorHost(const EditorHostInfo& info, Unique<Registries> registries,
                           Unique<LoadedModule> gameModule, optional<LoadedModule> editorModule)
        : Application(info.App, registries->Types, registries->Systems), m_Info(info),
          m_Registries(std::move(registries)), m_GameModule(std::move(gameModule)),
          m_EditorModule(std::move(editorModule))
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

    void EditorHost::OnInitialize()
    {
        VE_ASSERT(GetImGuiLayer() != nullptr, "editor host requires the ImGui layer");

        const VoidResult mount = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mount, "{}", mount.error());

        // The editor's own icon pack (light/camera billboard textures) sits beside the exe.
        // The engine ships no icon content; the viewport gizmos resolve their TextureHandles
        // from these. Mounting is best-effort — a missing pack only drops the gizmo icons.
        const VoidResult iconMount =
            GetAssetManager().Mount(ExecutableDirectory() / "editor_icons.vengpack");
        if (!iconMount)
        {
            Log::Warn("editor: icon pack not mounted: {}", iconMount.error());
        }

        // Parsed once; an empty index when no manifest is configured keeps the picker
        // candidate-free rather than absent.
        m_Sources = CreateUnique<AssetSourceIndex>(
            m_Info.AssetManifestPath ? AssetSourceIndex::Parse(*m_Info.AssetManifestPath)
                                     : AssetSourceIndex{});

        // The project-settings panel inspects ProjectSettings through reflection and draws the
        // two compression enums as named combos; register both before the panel is built.
        GetTypeRegistry().Register<ProjectSettings>();
        RegisterCompressionWidgets(m_Registries->Editor);

        // project.veng lives beside the manifest; load it (or stay at the empty zero-config
        // state) and remember the path so the panel saves there.
        if (m_Info.AssetManifestPath)
        {
            m_ProjectFile = m_Info.AssetManifestPath->parent_path() / "project.veng";
            m_ProjectSettings = LoadProjectSettings(m_ProjectFile);
        }

        // A prefab is edited live in a spawned Scene, so its editor needs no manifest
        // source; register it unconditionally.
        m_Registries->Editor.RegisterAssetEditor(
            AssetType::Prefab,
            CreateUnique<PrefabEditorFactory>(*this, GetAssetManager(), *GetImGuiLayer(),
                                              GetTypeRegistry(), m_Registries->Editor, *m_Sources,
                                              GetInput(), GetInputRouter(), GetSystemRegistry()));

        // try_emplace no-ops if the game module already registered a factory for these types.
        if (m_Info.AssetManifestPath)
        {
            auto cookFor = [this]
            {
                return VengEditor::CookDriver([this](const VengEditor::CookRequest& request,
                                                     function<void(Result<MountHandle>)> onComplete)
                                              { RequestCook(request, std::move(onComplete)); });
            };

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Texture,
                CreateUnique<TextureEditorFactory>(*m_Sources, GetRenderContext(),
                                                   GetAssetManager(), *GetImGuiLayer(), cookFor(),
                                                   [this] { return GetActiveConfiguration(); }));

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

        m_Panels.push_back({CreateUnique<AssetBrowserPanel>(
                                ExecutableDirectory() / "sample.vengpack", *m_Sources, *this),
                            true});
        m_Panels.push_back({CreateUnique<ConsolePanel>(), true});
        m_Panels.push_back(
            {CreateUnique<ProjectSettingsPanel>(m_ProjectSettings, m_ProjectFile, GetAssetManager(),
                                                m_Registries->Editor, *m_Sources),
             true});

        for (Unique<EditorPanel>& panel : m_Registries->Editor.Panels())
        {
            m_Panels.push_back({std::move(panel), true});
        }

        // Open the sample prefab as the initial document so the editor starts on live
        // content; double-clicking any prefab in the asset browser opens another.
        OpenAssetEditor(AssetType::Prefab, SampleScenePrefabId);
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

        // Inject the manifest path so the cook resolves cross-asset references by AssetId;
        // panels build manifest-agnostic requests.
        CookRequest resolved = request;
        if (m_Info.AssetManifestPath)
        {
            resolved.ReferenceManifest = *m_Info.AssetManifestPath;
        }

        // A level cook validates its system ids and config against the module's reflected
        // catalogs, so inject the game module path; non-level cooks ignore an empty value.
        resolved.ModulePath = m_Info.GameModulePath;

        // Thread the active configuration so a texture recook resolves roles the same way the
        // build does; a null active configuration leaves the request unset (zero-config cook).
        if (const BuildConfiguration* active = GetActiveConfiguration())
        {
            resolved.ActiveConfig = *active;
        }

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

        ImGui::DockSpaceOverViewport();
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
