#include "panels/InputMappingEditorPanel.h"

#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "FieldWidget.h"
#include "JsonUtil.h"

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/RawInput.h>
#include <Veng/Log.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <array>
#include <cstring>
#include <span>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        const char* PhaseName(ActionPhase phase)
        {
            switch (phase)
            {
            case ActionPhase::None:
                return "None";
            case ActionPhase::Started:
                return "Started";
            case ActionPhase::Ongoing:
                return "Ongoing";
            case ActionPhase::Completed:
                return "Completed";
            }
            return "?";
        }
    }

    InputMappingEditorPanel::InputMappingEditorPanel(AssetId id, path sourcePath,
                                                     AssetManager& assets,
                                                     const EditorRegistry& editors,
                                                     const AssetSourceIndex& sources,
                                                     const Input& input, CookDriver cook)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Assets(assets), m_Sources(sources),
          m_Editors(editors), m_Input(input), m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Input Map: {}", m_SourcePath.filename().string());

        LoadDocument();
        // Cook once on open so the asset is addressable behind the shadow mount; this reads the
        // source as authored and writes nothing.
        TriggerCook();
    }

    InputMappingEditorPanel::~InputMappingEditorPanel() = default;

    Result<InputMapData> ReadInputMapDocument(const path& source, const TypeRegistry& types)
    {
        const optional<nlohmann::json> doc = ReadJsonObject(source);
        if (!doc)
        {
            return std::unexpected(fmt::format("failed to read {}", source.string()));
        }
        InputMapData data;
        // Tolerant: a key naming no field (a hand-authored note) is skipped here and kept on save.
        const VoidResult bound =
            JsonReadFields(&data, types.Info(TypeIdOf<InputMapData>()), *doc, types, {}, true);
        if (!bound)
        {
            return std::unexpected(fmt::format("'{}': {}", source.string(), bound.error()));
        }
        return data;
    }

    VoidResult WriteInputMapDocument(const path& source, const InputMapData& data,
                                     const TypeRegistry& types)
    {
        return MergeWriteJsonObject(
            source, 2, [&](nlohmann::json& doc)
            { JsonWriteFields(doc, &data, types.Info(TypeIdOf<InputMapData>()), types); });
    }

    void InputMappingEditorPanel::LoadDocument()
    {
        m_Dirty = false;
        Result<InputMapData> loaded =
            ReadInputMapDocument(m_SourcePath, m_Assets.GetTypeRegistry());
        if (!loaded)
        {
            m_Doc = InputMapData{};
            Log::Error("Input map editor: {}", loaded.error());
            return;
        }
        m_Doc = std::move(*loaded);
    }

    VoidResult InputMappingEditorPanel::WriteDocument()
    {
        const VoidResult written =
            WriteInputMapDocument(m_SourcePath, m_Doc, m_Assets.GetTypeRegistry());
        if (!written)
        {
            m_CookError = written.error();
            Log::Error("Input map editor: {}", written.error());
        }
        return written;
    }

    VoidResult InputMappingEditorPanel::Save()
    {
        return SaveAssetSource([this] { return WriteDocument(); }, m_Dirty,
                               [this] { TriggerCook(); });
    }

    void InputMappingEditorPanel::TriggerCook()
    {
        m_Gate.Request(
            [this]
            {
                m_CookError.reset();

                m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetTypes::InputMap},
                       [this](Result<MountHandle> mount)
                       {
                           if (!mount)
                           {
                               m_CookError = mount.error();
                           }
                           else
                           {
                               // Replace the mount and reload behind the stable handle: a running
                               // Play session in the editor picks up the new bindings the moment the
                               // reload lands resident.
                               m_Mount = std::move(*mount);
                               m_Handle = m_Assets.Load<InputMappingContext>(m_Id);
                           }
                           m_Gate.Complete();
                       });
            });
    }

    bool InputMappingEditorPanel::DrawActionCombo(void* fieldPtr) const
    {
        ActionId current{};
        std::memcpy(&current, fieldPtr, sizeof(current));

        // Build the label list from the document's declared actions: index 0 is "(none)", index
        // N+1 names action N. An id matching no declared action shows a synthesized "(unknown)"
        // entry so the drift is visible; picking a named entry repairs it.
        vector<string> labels;
        labels.reserve(m_Doc.Actions.size() + 2);
        labels.emplace_back("(none)");
        for (const InputAction& action : m_Doc.Actions)
        {
            labels.push_back(action.Name.empty()
                                 ? fmt::format("(action {})", static_cast<u64>(action.Id))
                                 : action.Name);
        }

        i32 index = 0;
        if (current != ActionId::Null)
        {
            for (usize i = 0; i < m_Doc.Actions.size(); ++i)
            {
                if (m_Doc.Actions[i].Id == current)
                {
                    index = static_cast<i32>(i) + 1;
                    break;
                }
            }
            if (index == 0)
            {
                labels.push_back(fmt::format("(unknown {})", static_cast<u64>(current)));
                index = static_cast<i32>(labels.size()) - 1;
            }
        }

        const vector<string_view> items(labels.begin(), labels.end());
        if (!UI::Combo("##actionid", index, items))
        {
            return false;
        }
        // Re-picking the unknown row keeps the id it names.
        ActionId chosen = current;
        if (index == 0)
        {
            chosen = ActionId::Null;
        }
        else if (static_cast<usize>(index) <= m_Doc.Actions.size())
        {
            chosen = m_Doc.Actions[static_cast<usize>(index) - 1].Id;
        }
        if (chosen == current)
        {
            return false;
        }
        std::memcpy(fieldPtr, &chosen, sizeof(chosen));
        return true;
    }

    void InputMappingEditorPanel::DrawPreview()
    {
        UI::SeparatorText("Preview (this editor's input)");

        // Rebuild the resolver-ready form each frame from the live document, then resolve it over
        // the editor host's always-fed input snapshot (read through the public RawInput adapter).
        // Threading last frame's ActionState as `previous` derives each action's phase. A keystroke
        // captured by an active text widget in this panel does not reach the snapshot.
        m_Resolved.Actions = m_Doc.Actions;
        m_Resolved.Bindings = m_Doc.Bindings;

        const RawInput raw(m_Input);
        const std::array<ResolvedContext, 1> active{m_Resolved};
        const ActionState state = ResolveActions(active, raw, m_Previous);
        m_Previous = state;

        if (state.Actions.empty())
        {
            UI::TextDisabled("No actions declared");
            return;
        }

        if (const auto table = UI::PropertyTable("##inputmappreview"))
        {
            for (const ActionSample& sample : state.Actions)
            {
                string name = fmt::format("{}", static_cast<u64>(sample.Id));
                for (const InputAction& action : m_Doc.Actions)
                {
                    if (action.Id == sample.Id && !action.Name.empty())
                    {
                        name = action.Name;
                        break;
                    }
                }
                UI::PropertyLabel(name);
                UI::Text(fmt::format("({: .2f}, {: .2f})  {}", sample.Value.x, sample.Value.y,
                                     PhaseName(sample.Phase)));
            }
        }
    }

    void InputMappingEditorPanel::OnUI()
    {
        if (m_Gate.IsCooking())
        {
            UI::Text("Cooking...");
        }
        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }

        DrawPreview();

        UI::Separator();

        const TypeRegistry& types = m_Assets.GetTypeRegistry();
        const TypeInfo& info = types.Info(types.IdOf<InputMapData>());

        // Reflection draws both arrays: the ActionId combo makes each binding's action
        // readable/pickable by name, the VE_ENUM combos handle device/kind/axis, and the
        // FieldClass::Array add/remove widget makes the table editable. The combo names this
        // document's actions, so it is this walk's override rather than a registry widget, which
        // would draw in every inspector and outlive the panel.
        const std::array<FieldWidgetOverride, 1> widgets{{
            {.Type = TypeIdOf<ActionId>(),
             .Widget = [this](void* fieldPtr, const FieldDescriptor&)
             { return DrawActionCombo(fieldPtr); }},
        }};
        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors, .Overrides = widgets};
        if (const auto table = UI::PropertyTable("##inputmap"))
        {
            if (DrawFields(&m_Doc, info.Fields, ctx))
            {
                m_Dirty = true;
            }
        }

        UI::Separator();

        if (const auto bar = UI::Toolbar("##inputmap-toolbar"))
        {
            {
                const UI::DisabledScope disabled = UI::Disabled(!m_Dirty);
                if (UI::IconButton(Icons::Save))
                {
                    if (const VoidResult saved = Save(); !saved)
                    {
                        Log::Error("Input map editor: save failed: {}", saved.error());
                    }
                }
                UI::Tooltip("Save the input map to its .inputmap.json and recook");
            }
            UI::SameLine();
            if (UI::IconButton(Icons::Revert))
            {
                LoadDocument();
                TriggerCook();
            }
            UI::Tooltip("Discard edits and reload the input map from disk");
        }
    }

    vector<Inspectable> InputMappingEditorPanel::GetInspectables()
    {
        return {
            Inspectable{.Name = "inputMap", .Type = TypeIdOf<InputMapData>(), .Data = &m_Doc},
        };
    }

    void InputMappingEditorPanel::OnInspectableChanged(string_view name)
    {
        if (name == "inputMap")
        {
            // An external write lands the same way a UI edit does: it marks the document dirty and
            // waits for a save, which the MCP surface reaches through editor.save.
            m_Dirty = true;
        }
    }
}
