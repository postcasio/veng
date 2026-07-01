#include "panels/LevelEditorPanel.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include "AssetSourceIndex.h"
#include "FieldWidget.h"
#include "panels/SceneViewportPanel.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The active-set drag-reorder payload: the index being dragged, matched within the
        // systems panel only.
        constexpr string_view SystemReorderPayload = "VENG_LEVEL_SYSTEM";

        const char* PhaseName(SceneSystem::Phase phase)
        {
            return phase == SceneSystem::Phase::View ? "View" : "Sim";
        }

        // The level's gameMode/render config blocks are tolerant reflection records (scalars,
        // vectors, asset handles, nested structs). These two helpers round-trip one such block
        // through nlohmann, mirroring the cooker's LevelImporter read and libveng's WriteFields —
        // so the editor writes exactly what the cooker reads. Unknown source keys are left to the
        // caller's preserve-unknown-keys merge; an omitted field keeps its default (schema
        // tolerance), an unsupported field class is skipped.

        void ReadConfigObject(const nlohmann::json& object, void* obj, const TypeInfo& info,
                              const TypeRegistry& types)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (!object.contains(field.Name))
                {
                    continue;
                }
                const nlohmann::json& value = object[field.Name];
                void* fieldPtr = static_cast<u8*>(obj) + field.Offset;
                switch (field.Class)
                {
                case FieldClass::Scalar:
                {
                    if (field.Type == TypeIdOf<bool>() && value.is_boolean())
                    {
                        const bool v = value.get<bool>();
                        std::memcpy(fieldPtr, &v, sizeof(v));
                    }
                    else if (field.Type == TypeIdOf<f32>() && value.is_number())
                    {
                        const f32 v = value.get<f32>();
                        std::memcpy(fieldPtr, &v, sizeof(v));
                    }
                    else if (field.Type == TypeIdOf<i32>() && value.is_number())
                    {
                        const i32 v = value.get<i32>();
                        std::memcpy(fieldPtr, &v, sizeof(v));
                    }
                    break;
                }
                case FieldClass::Vector:
                {
                    const bool unsignedVector = field.Type == TypeIdOf<uvec2>();
                    const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
                    const usize arity = types.Info(field.Type).Size / componentSize;
                    if (!value.is_array() || value.size() != arity)
                    {
                        break;
                    }
                    for (usize i = 0; i < arity; ++i)
                    {
                        if (unsignedVector)
                        {
                            const u32 v = value[i].get<u32>();
                            std::memcpy(static_cast<u8*>(fieldPtr) + i * sizeof(u32), &v,
                                        sizeof(v));
                        }
                        else
                        {
                            const f32 v = value[i].get<f32>();
                            std::memcpy(static_cast<u8*>(fieldPtr) + i * sizeof(f32), &v,
                                        sizeof(v));
                        }
                    }
                    break;
                }
                case FieldClass::AssetHandle:
                {
                    if (value.is_number_unsigned())
                    {
                        const u64 v = value.get<u64>();
                        std::memcpy(fieldPtr, &v, sizeof(v));
                    }
                    break;
                }
                case FieldClass::Struct:
                {
                    if (value.is_object())
                    {
                        ReadConfigObject(value, fieldPtr, types.Info(field.Type), types);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        void WriteConfigObject(nlohmann::json& object, const void* obj, const TypeInfo& info,
                               const TypeRegistry& types)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                const auto* fieldPtr = static_cast<const u8*>(obj) + field.Offset;
                switch (field.Class)
                {
                case FieldClass::Scalar:
                {
                    if (field.Type == TypeIdOf<bool>())
                    {
                        bool v = false;
                        std::memcpy(&v, fieldPtr, sizeof(v));
                        object[field.Name] = v;
                    }
                    else if (field.Type == TypeIdOf<f32>())
                    {
                        f32 v = 0.0f;
                        std::memcpy(&v, fieldPtr, sizeof(v));
                        object[field.Name] = v;
                    }
                    else if (field.Type == TypeIdOf<i32>())
                    {
                        i32 v = 0;
                        std::memcpy(&v, fieldPtr, sizeof(v));
                        object[field.Name] = v;
                    }
                    break;
                }
                case FieldClass::Vector:
                {
                    const bool unsignedVector = field.Type == TypeIdOf<uvec2>();
                    const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
                    const usize arity = types.Info(field.Type).Size / componentSize;
                    nlohmann::json array = nlohmann::json::array();
                    for (usize i = 0; i < arity; ++i)
                    {
                        if (unsignedVector)
                        {
                            u32 v = 0;
                            std::memcpy(&v, fieldPtr + i * sizeof(u32), sizeof(v));
                            array.push_back(v);
                        }
                        else
                        {
                            f32 v = 0.0f;
                            std::memcpy(&v, fieldPtr + i * sizeof(f32), sizeof(v));
                            array.push_back(v);
                        }
                    }
                    object[field.Name] = std::move(array);
                    break;
                }
                case FieldClass::AssetHandle:
                {
                    u64 v = 0;
                    std::memcpy(&v, fieldPtr, sizeof(v));
                    object[field.Name] = v;
                    break;
                }
                case FieldClass::Struct:
                {
                    nlohmann::json& nested = object[field.Name];
                    if (!nested.is_object())
                    {
                        nested = nlohmann::json::object();
                    }
                    WriteConfigObject(nested, fieldPtr, types.Info(field.Type), types);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    // A child panel that forwards its body back to the owning level editor, so the systems
    // and settings panels are ordinary docked children without each needing its own class.
    class LevelEditorPanel::LevelChildPanel final : public EditorPanel
    {
    public:
        LevelChildPanel(string title, function<void()> draw)
            : m_Title(std::move(title)), m_Draw(std::move(draw))
        {
        }

        [[nodiscard]] string_view GetTitle() const override { return m_Title; }
        void OnUI() override { m_Draw(); }

    private:
        string m_Title;
        function<void()> m_Draw;
    };

    LevelEditorPanel::LevelEditorPanel(AssetId id, AssetId worldPrefab, path sourcePath,
                                       Application& app, AssetManager& assets, ImGuiLayer& imgui,
                                       TypeRegistry& types, EditorRegistry& editors,
                                       const AssetSourceIndex& sources, Input& input,
                                       InputRouter& router, SystemRegistry& systems,
                                       CookDriver cook)
        : PrefabEditorPanel(worldPrefab, fmt::format("Level 0x{:X}", id.Value), app, assets, imgui,
                            types, editors, sources, input, router, systems),
          m_Id(id), m_SourcePath(std::move(sourcePath)), m_AssetManager(assets), m_Catalog(systems),
          m_Editors(editors), m_Sources(sources), m_Cook(std::move(cook))
    {
        LoadConfig();

        // The level's entity edits save back to its referenced world prefab's .prefab.json (its
        // own .level.json holds only the system set + config). Resolve that prefab source so the
        // inherited Save() persists layout edits there; an unindexed world prefab disables it.
        if (const AssetSourceIndex::Entry* entry = sources.Find(worldPrefab))
        {
            m_PrefabSource = entry->Source;
        }

        // The world prefab is the scene surface; add the same viewport/explorer/inspector a
        // standalone prefab editor uses, then the two level-scoped children.
        AddSceneEditingChildren(app, imgui, editors, sources);

        // Seed the viewport with the level's authored render subset so the first frame already
        // renders the level's exposure/bloom/shadow config, not the viewport defaults.
        m_Viewport->ApplyLevelRenderSettings(m_Render);

        m_SystemsChild =
            AddChild(CreateUnique<LevelChildPanel>("Systems", [this] { DrawSystemsPanel(); }));
        m_SettingsChild = AddChild(
            CreateUnique<LevelChildPanel>("Level Settings", [this] { DrawSettingsPanel(); }));

        m_Handle = m_AssetManager.Load<Level>(m_Id);
    }

    LevelEditorPanel::~LevelEditorPanel() = default;

    void LevelEditorPanel::SeedPlayScene(Scene& scene)
    {
        // The player prefab is usually already a resolved dependency of the world prefab, so
        // the common case takes neither branch; force it resident otherwise, since the spawn
        // rule bails on a non-resident prefab the way the runtime relies on the loader having
        // eager-resolved it.
        if (!m_GameMode.PlayerPrefab.IsLoaded())
        {
            const AssetResult<AssetHandle<Prefab>> player =
                m_AssetManager.LoadSync<Prefab>(m_GameMode.PlayerPrefab.Id());
            if (!player.has_value())
            {
                Log::Error("Level editor: failed to load player prefab 0x{:X}: {}",
                           m_GameMode.PlayerPrefab.Id().Value, player.error().Detail);
            }
            else
            {
                m_GameMode.PlayerPrefab = *player;
            }
        }

        SeedLevel(scene, m_GameMode, m_Render);
    }

    void LevelEditorPanel::LoadConfig()
    {
        m_Systems.clear();
        m_GameMode = GameModeConfig{};
        m_Render = LevelRenderSettings{};

        const std::ifstream file(m_SourcePath, std::ios::binary);
        if (!file)
        {
            Log::Error("Level editor: failed to open {}", m_SourcePath.string());
            return;
        }

        std::ostringstream contents;
        contents << file.rdbuf();
        const nlohmann::json level = nlohmann::json::parse(contents.str(), nullptr, false);
        if (level.is_discarded() || !level.is_object())
        {
            Log::Error("Level editor: malformed JSON {}", m_SourcePath.string());
            return;
        }

        if (level.contains("systems") && level["systems"].is_array())
        {
            for (const nlohmann::json& sysId : level["systems"])
            {
                if (sysId.is_number_unsigned())
                {
                    m_Systems.push_back(sysId.get<u64>());
                }
            }
        }

        const TypeRegistry& types = m_Context.Scene->GetTypeRegistry();
        auto readConfig = [&](const char* key, void* obj, TypeId type)
        {
            if (!level.contains(key) || !level[key].is_object())
            {
                return;
            }
            ReadConfigObject(level[key], obj, types.Info(type), types);
        };

        readConfig("gameMode", &m_GameMode, TypeIdOf<GameModeConfig>());
        readConfig("render", &m_Render, TypeIdOf<LevelRenderSettings>());
    }

    bool LevelEditorPanel::SaveConfig()
    {
        // Read the existing file so unknown keys (the world id, hand-authored structure)
        // survive the round-trip — only the edited sections are patched.
        nlohmann::json level = nlohmann::json::object();
        {
            const std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                {
                    level = parsed;
                }
            }
        }

        nlohmann::json systems = nlohmann::json::array();
        for (const SystemId sysId : m_Systems)
        {
            systems.push_back(sysId);
        }
        level["systems"] = std::move(systems);

        const TypeRegistry& types = m_Context.Scene->GetTypeRegistry();
        auto writeConfig = [&](const char* key, const void* obj, TypeId type)
        {
            nlohmann::json& object = level[key];
            if (!object.is_object())
            {
                object = nlohmann::json::object();
            }
            WriteConfigObject(object, obj, types.Info(type), types);
        };

        writeConfig("gameMode", &m_GameMode, TypeIdOf<GameModeConfig>());
        writeConfig("render", &m_Render, TypeIdOf<LevelRenderSettings>());

        std::ofstream out(m_SourcePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            m_CookError = fmt::format("failed to write {}", m_SourcePath.string());
            Log::Error("Level editor: {}", *m_CookError);
            return false;
        }
        out << level.dump(2) << '\n';
        return true;
    }

    void LevelEditorPanel::TriggerCook()
    {
        if (m_Cooking)
        {
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetType::Level},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (!mount)
                   {
                       m_CookError = mount.error();
                       return;
                   }

                   // Replace the mount and re-fetch behind the stable handle; the reloaded
                   // Level reflects the recooked systems/config the next time it is read.
                   m_Mount = std::move(*mount);
                   m_Handle = m_AssetManager.Load<Level>(m_Id);
               });
    }

    void LevelEditorPanel::MarkDirty()
    {
        // Config edits (systems, game-mode, render) accumulate in memory and show live in the
        // viewport (DrawSettingsPanel pushes the render subset directly); they persist and recook
        // only on Save, so nothing touches the source file per-frame.
        m_ConfigDirty = true;
    }

    VoidResult LevelEditorPanel::Save()
    {
        // The level spans two sources: the world prefab's .prefab.json (its entity layout, saved
        // through the base) and its own .level.json (systems + config). Persist whichever is dirty,
        // then recook so the mounted Level reflects the config edits. A clean half is skipped; the
        // prefab failure aborts before the config write.
        if (m_Commands.IsDirty())
        {
            const VoidResult prefab = PrefabEditorPanel::Save();
            if (!prefab)
            {
                return prefab;
            }
        }
        if (m_ConfigDirty)
        {
            if (!SaveConfig())
            {
                return std::unexpected(m_CookError.value_or(string{"failed to save level config"}));
            }
            m_ConfigDirty = false;
            TriggerCook();
        }
        return {};
    }

    void LevelEditorPanel::OnUI()
    {
        // This overrides the base OnUI, so it owns driving the play tick the base would have run.
        TickPlaySimulation();

        if (auto bar = UI::Toolbar("##level-toolbar"))
        {
            DrawDocumentToolbar();
            UI::SameLine();
            UI::Separator();
            UI::SameLine();

            // The level runs its named set, distinct from a prefab document's "all registered"
            // set; surface which is active so Play is not surprising.
            UI::TextDisabled(fmt::format("Play runs {} level systems", m_Systems.size()));
            if (m_Cooking)
            {
                UI::SameLine();
                UI::TextDisabled("(cooking…)");
            }
        }
        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }
    }

    void LevelEditorPanel::DrawSystemsPanel()
    {
        UI::SeparatorText("Active systems");
        UI::TextDisabled("Order is run order; drag to reorder. Enable a system to add it.");

        bool changed = false;

        // The active ordered set, each row a drag source + drop target for reordering.
        usize dragFrom = m_Systems.size();
        usize dragTo = m_Systems.size();
        for (usize i = 0; i < m_Systems.size(); ++i)
        {
            const SystemId sysId = m_Systems[i];

            string name = fmt::format("0x{:X}", sysId);
            optional<SceneSystem::Phase> phase;
            for (const SystemEntry& entry : m_Catalog.Entries())
            {
                if (entry.Id == sysId)
                {
                    name = entry.Name;
                    if (const Unique<SceneSystem> instance = entry.Factory())
                    {
                        phase = instance->GetPhase();
                    }
                    break;
                }
            }

            auto rowId = UI::PushId(fmt::format("active{}", i));

            const string label =
                fmt::format("{}. {}  [{}]", i + 1, name, phase ? PhaseName(*phase) : "?");
            (void)UI::Selectable(label);

            if (auto source = UI::DragDropSource())
            {
                UI::SetDragDropPayload(SystemReorderPayload, &i, sizeof(i));
                UI::Text(name);
            }
            if (auto target = UI::DragDropTarget())
            {
                if (const void* payload = UI::AcceptDragDropPayload(SystemReorderPayload))
                {
                    usize from = 0;
                    std::memcpy(&from, payload, sizeof(from));
                    dragFrom = from;
                    dragTo = i;
                }
            }
        }

        if (dragFrom < m_Systems.size() && dragTo < m_Systems.size() && dragFrom != dragTo)
        {
            const SystemId moved = m_Systems[dragFrom];
            m_Systems.erase(m_Systems.begin() + static_cast<std::ptrdiff_t>(dragFrom));
            m_Systems.insert(m_Systems.begin() + static_cast<std::ptrdiff_t>(dragTo), moved);
            changed = true;
        }

        UI::Dummy(vec2{0.0f, 4.0f});
        UI::SeparatorText("Catalog");

        // Every registered system, each with an enable toggle that adds/removes it from the
        // active set; the catalog drives the list, so a game's own systems appear here.
        for (const SystemEntry& entry : m_Catalog.Entries())
        {
            auto rowId = UI::PushId(fmt::format("cat{:X}", entry.Id));

            const auto active = std::ranges::find(m_Systems, entry.Id);
            bool enabled = active != m_Systems.end();

            optional<SceneSystem::Phase> phase;
            if (const Unique<SceneSystem> instance = entry.Factory())
            {
                phase = instance->GetPhase();
            }

            if (UI::Checkbox("##enable", enabled))
            {
                if (enabled)
                {
                    m_Systems.push_back(entry.Id);
                }
                else if (active != m_Systems.end())
                {
                    m_Systems.erase(active);
                }
                changed = true;
            }
            UI::SameLine();
            UI::Text(fmt::format("{}  [{}]", entry.Name, phase ? PhaseName(*phase) : "?"));
        }

        if (changed)
        {
            MarkDirty();
        }
    }

    void LevelEditorPanel::DrawSettingsPanel()
    {
        const TypeRegistry& types = m_Context.Scene->GetTypeRegistry();
        const FieldWidgetContext ctx{
            .Assets = m_AssetManager, .Sources = m_Sources, .Editors = m_Editors};

        bool changed = false;

        auto drawConfig = [&](string_view title, void* obj, TypeId type)
        {
            UI::SeparatorText(title);
            const TypeInfo& info = types.Info(type);
            if (auto table = UI::PropertyTable(fmt::format("##{}", info.Name)))
            {
                changed |= DrawFields(obj, info.Fields, ctx);
            }
        };

        drawConfig("Game mode", &m_GameMode, TypeIdOf<GameModeConfig>());
        UI::Dummy(vec2{0.0f, 4.0f});
        drawConfig("Render", &m_Render, TypeIdOf<LevelRenderSettings>());

        if (changed)
        {
            // Push the live render subset to the viewport so the edit shows immediately, ahead
            // of the debounced recook; m_GameMode edits do not touch rendering.
            m_Viewport->ApplyLevelRenderSettings(m_Render);
            MarkDirty();
        }
    }

    vector<Inspectable> LevelEditorPanel::GetInspectables()
    {
        return {
            Inspectable{.Name = "renderSettings",
                        .Type = TypeIdOf<LevelRenderSettings>(),
                        .Data = &m_Render},
            Inspectable{
                .Name = "gameMode", .Type = TypeIdOf<GameModeConfig>(), .Data = &m_GameMode},
        };
    }

    void LevelEditorPanel::OnInspectableChanged(string_view name)
    {
        if (name == "renderSettings")
        {
            // A render-settings write previews live in the viewport ahead of the debounced recook,
            // exactly as a DrawSettingsPanel edit does; a game-mode write only marks dirty.
            m_Viewport->ApplyLevelRenderSettings(m_Render);
        }
        MarkDirty();
    }

    void LevelEditorPanel::BuildDefaultLayout(u32 dockspaceId)
    {
        ImGuiID center = dockspaceId;
        const ImGuiID left =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
        const ImGuiID right =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);

        // Level settings, systems and the hierarchy share the left node as tabs (the
        // hierarchy docked last so it is the active tab); the inspector fills the right; the
        // viewport fills the center.
        DockChildWindow(m_SettingsChild, left);
        DockChildWindow(m_SystemsChild, left);
        DockChildWindow(m_ExplorerChild, left);
        DockChildWindow(m_ViewportChild, center);
        DockChildWindow(m_InspectorChild, right);
    }
}
