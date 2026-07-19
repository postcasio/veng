#include "panels/InputMappingEditorPanel.h"

#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "FieldWidget.h"
#include "JsonUtil.h"

#include <Veng/Asset/HexId.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/RawInput.h>
#include <Veng/Log.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>

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
                                                     AssetManager& assets, EditorRegistry& editors,
                                                     const AssetSourceIndex& sources,
                                                     const Input& input, CookDriver cook)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Assets(assets), m_Sources(sources),
          m_Editors(editors), m_Input(input), m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Input Map: {}", m_SourcePath.filename().string());

        // The one custom widget: an ActionId is a u64 leaf with no default scalar widget, so
        // register a name combo scoped to this document's declared actions. Registered on the
        // shared EditorRegistry keyed by TypeId, so it draws every ActionId field this panel walks.
        editors.RegisterFieldWidget(TypeIdOf<ActionId>(),
                                    [this](void* fieldPtr, const FieldDescriptor&)
                                    { DrawActionCombo(fieldPtr); });

        LoadDocument();
        // Cook once on open so the asset is addressable behind the shadow mount; this reads the
        // source as authored and writes nothing.
        TriggerCook();
    }

    InputMappingEditorPanel::~InputMappingEditorPanel() = default;

    void InputMappingEditorPanel::LoadDocument()
    {
        m_Doc = InputMapData{};
        m_Dirty = false;

        const optional<nlohmann::json> docResult = ReadJsonObject(m_SourcePath);
        if (!docResult)
        {
            Log::Error("Input map editor: failed to read {}", m_SourcePath.string());
            return;
        }
        const nlohmann::json& doc = *docResult;

        if (doc.contains("actions") && doc["actions"].is_array())
        {
            for (const nlohmann::json& actionJson : doc["actions"])
            {
                if (!actionJson.is_object())
                {
                    continue;
                }
                InputAction action;
                if (actionJson.contains("id"))
                {
                    if (!actionJson["id"].is_string())
                    {
                        Log::Error("Input map editor: '{}': an action 'id' must be a hex id string",
                                   m_SourcePath.string());
                    }
                    else if (const optional<u64> parsed =
                                 ParseHexId(actionJson["id"].get<std::string>()))
                    {
                        action.Id = static_cast<ActionId>(*parsed);
                    }
                    else
                    {
                        Log::Error(
                            "Input map editor: '{}': an action 'id' is a malformed hex id '{}'",
                            m_SourcePath.string(), actionJson["id"].get<std::string>());
                    }
                }
                if (actionJson.contains("name") && actionJson["name"].is_string())
                {
                    action.Name = actionJson["name"].get<std::string>();
                }
                if (actionJson.contains("kind") && actionJson["kind"].is_string())
                {
                    if (auto kind = ParseEnum<ActionKind>(actionJson["kind"].get<std::string>()))
                    {
                        action.Kind = *kind;
                    }
                }
                m_Doc.Actions.push_back(std::move(action));
            }
        }

        if (doc.contains("bindings") && doc["bindings"].is_array())
        {
            for (const nlohmann::json& bindingJson : doc["bindings"])
            {
                if (!bindingJson.is_object())
                {
                    continue;
                }
                Binding binding;
                if (bindingJson.contains("source") && bindingJson["source"].is_object())
                {
                    const nlohmann::json& sourceJson = bindingJson["source"];
                    if (sourceJson.contains("device") && sourceJson["device"].is_string())
                    {
                        if (auto device =
                                ParseEnum<InputDeviceType>(sourceJson["device"].get<std::string>()))
                        {
                            binding.Source.Device = *device;
                        }
                    }
                    if (sourceJson.contains("control") &&
                        sourceJson["control"].is_number_unsigned())
                    {
                        binding.Source.Control = sourceJson["control"].get<u32>();
                    }
                }
                if (bindingJson.contains("action"))
                {
                    if (!bindingJson["action"].is_string())
                    {
                        Log::Error(
                            "Input map editor: '{}': a binding 'action' must be a hex id string",
                            m_SourcePath.string());
                    }
                    else if (const optional<u64> parsed =
                                 ParseHexId(bindingJson["action"].get<std::string>()))
                    {
                        binding.Action = static_cast<ActionId>(*parsed);
                    }
                    else
                    {
                        Log::Error(
                            "Input map editor: '{}': a binding 'action' is a malformed hex id '{}'",
                            m_SourcePath.string(), bindingJson["action"].get<std::string>());
                    }
                }
                if (bindingJson.contains("axis") && bindingJson["axis"].is_string())
                {
                    if (auto axis =
                            ParseEnum<AxisComponent>(bindingJson["axis"].get<std::string>()))
                    {
                        binding.Axis = *axis;
                    }
                }
                if (bindingJson.contains("scale") && bindingJson["scale"].is_number())
                {
                    binding.Scale = bindingJson["scale"].get<f32>();
                }
                m_Doc.Bindings.push_back(binding);
            }
        }
    }

    VoidResult InputMappingEditorPanel::WriteDocument()
    {
        // Round-trip the existing file so unknown keys (a hand-authored comment field, per-map
        // settings no widget exposes) survive; only the actions/bindings arrays are rewritten.
        const VoidResult written =
            MergeWriteJsonObject(m_SourcePath, 2,
                                 [this](nlohmann::json& doc)
                                 {
                                     nlohmann::json actions = nlohmann::json::array();
                                     for (const InputAction& action : m_Doc.Actions)
                                     {
                                         nlohmann::json entry = nlohmann::json::object();
                                         entry["id"] = FormatHexId(static_cast<u64>(action.Id));
                                         entry["name"] = action.Name;
                                         entry["kind"] = EnumeratorName(action.Kind);
                                         actions.push_back(std::move(entry));
                                     }
                                     doc["actions"] = std::move(actions);

                                     nlohmann::json bindings = nlohmann::json::array();
                                     for (const Binding& binding : m_Doc.Bindings)
                                     {
                                         nlohmann::json source = nlohmann::json::object();
                                         source["device"] = EnumeratorName(binding.Source.Device);
                                         source["control"] = binding.Source.Control;

                                         nlohmann::json entry = nlohmann::json::object();
                                         entry["source"] = std::move(source);
                                         entry["action"] =
                                             FormatHexId(static_cast<u64>(binding.Action));
                                         entry["axis"] = EnumeratorName(binding.Axis);
                                         entry["scale"] = binding.Scale;
                                         bindings.push_back(std::move(entry));
                                     }
                                     doc["bindings"] = std::move(bindings);
                                 });

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

    void InputMappingEditorPanel::DrawActionCombo(void* fieldPtr)
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
        if (UI::Combo("##actionid", index, items))
        {
            ActionId chosen = ActionId::Null;
            if (index >= 1 && static_cast<usize>(index) <= m_Doc.Actions.size())
            {
                chosen = m_Doc.Actions[static_cast<usize>(index) - 1].Id;
            }
            std::memcpy(fieldPtr, &chosen, sizeof(chosen));
            m_Dirty = true;
        }
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

        if (auto table = UI::PropertyTable("##inputmappreview"))
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

        // Reflection draws both arrays: the registered ActionId combo makes each binding's action
        // readable/pickable by name, the VE_ENUM combos handle device/kind/axis, and the
        // FieldClass::Array add/remove widget makes the table editable.
        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        bool changed = false;
        if (auto table = UI::PropertyTable("##inputmap"))
        {
            changed = DrawFields(&m_Doc, info.Fields, ctx);
        }

        if (changed)
        {
            // The ActionId combo marks itself dirty inline: its void widget signature carries no
            // change signal to this walk.
            m_Dirty = true;
        }

        UI::Separator();

        if (auto bar = UI::Toolbar("##inputmap-toolbar"))
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
