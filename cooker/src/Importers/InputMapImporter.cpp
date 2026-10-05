#include "InputMapImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

namespace Veng::Cook
{
    namespace
    {
        // Located-error prefix for an input-map field.
        string Located(const string& file, const string& reason)
        {
            return fmt::format("input map importer: '{}': {}", file, reason);
        }

        // Why a source names no readable control, or empty when it does. A gamepad control must index
        // a real button or axis; every other device's code space is open-ended.
        string CheckControl(const InputSource& source)
        {
            if (source.Device == InputDeviceType::GamepadButton &&
                source.Control >= static_cast<u32>(GamepadButton::Count))
            {
                return fmt::format("gamepad button {} is out of range (there are {})",
                                   source.Control, static_cast<u32>(GamepadButton::Count));
            }
            if (source.Device == InputDeviceType::GamepadAxis &&
                source.Control >= static_cast<u32>(GamepadAxis::Count))
            {
                return fmt::format("gamepad axis {} is out of range (there are {})", source.Control,
                                   static_cast<u32>(GamepadAxis::Count));
            }
            return {};
        }

        // Whether a device reads exactly 0 or 1, so a modifier on it is down at any threshold up to 1.
        bool IsDigital(const InputDeviceType device)
        {
            return device == InputDeviceType::Keyboard || device == InputDeviceType::MouseButton ||
                   device == InputDeviceType::GamepadButton;
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }
    }

    Result<vector<u8>> InputMapImporter::Cook(const CookContext& context, const json& entry) const
    {
        // --- 1. Read + parse the external *.inputmap.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("input map importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string file = sourcePath.string();

        const Result<json> docResult = ReadJsonFile(sourcePath, "input map importer");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }
        const json& doc = *docResult;

        // The context references only engine builtins (InputAction / Binding and their enums), so
        // it needs no game module: build a builtin-only registry for the walker + WriteFields.
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);

        // --- 2. Bind the reflected document ---

        // The source is the reflected InputMapData itself, keyed by field name, so a field added to
        // InputAction or Binding authors with no importer change.
        InputMapData data;
        const VoidResult bound =
            JsonReadFields(&data, registry.Info(TypeIdOf<InputMapData>()), doc, registry);
        if (!bound)
        {
            return std::unexpected(Located(file, bound.error()));
        }

        // --- 3. Validate ---

        // Track declared action ids for the binding cross-check and for the uniqueness rule.
        map<u64, ActionKind> declared;
        for (const InputAction& action : data.Actions)
        {
            const auto id = static_cast<u64>(action.Id);
            if (id == 0)
            {
                return std::unexpected(Located(file, "an action 'Id' must be non-null"));
            }
            if (!declared.emplace(id, action.Kind).second)
            {
                return std::unexpected(Located(
                    file, fmt::format("action id {} is declared more than once", FormatHexId(id))));
            }

            // A role fires on a press, so its action is a Button; a stick drives one through a
            // binding's Threshold. Repeat is the engine's timing of a held role, so it means nothing
            // on an action the engine never acts on, and a delay with no rate never repeats.
            if (action.Role != ActionRole::None && action.Kind != ActionKind::Button)
            {
                return std::unexpected(Located(
                    file, fmt::format("action {} has Role '{}' but Kind '{}'; a role action is a "
                                      "Button",
                                      FormatHexId(id), EnumeratorName(action.Role),
                                      EnumeratorName(action.Kind))));
            }
            if (!(action.RepeatDelay >= 0.0f) || !(action.RepeatRate >= 0.0f) ||
                !std::isfinite(action.RepeatDelay) || !std::isfinite(action.RepeatRate))
            {
                return std::unexpected(Located(
                    file, fmt::format("action {} has 'RepeatDelay' {} and 'RepeatRate' {}; both "
                                      "are seconds, 0 or more",
                                      FormatHexId(id), action.RepeatDelay, action.RepeatRate)));
            }
            const bool repeats = action.RepeatDelay > 0.0f || action.RepeatRate > 0.0f;
            if (repeats && action.Role == ActionRole::None)
            {
                return std::unexpected(Located(
                    file, fmt::format("action {} sets a repeat but has no Role; only a role action "
                                      "repeats",
                                      FormatHexId(id))));
            }
            if (action.RepeatDelay > 0.0f && action.RepeatRate == 0.0f)
            {
                return std::unexpected(Located(
                    file, fmt::format("action {} sets 'RepeatDelay' {} with no 'RepeatRate', so it "
                                      "never repeats",
                                      FormatHexId(id), action.RepeatDelay)));
            }
        }

        for (const Binding& binding : data.Bindings)
        {
            const auto actionId = static_cast<u64>(binding.Action);

            // The typo-catch a global registry would otherwise miss: a binding must name an
            // action this context declares.
            const auto declaration = declared.find(actionId);
            if (declaration == declared.end())
            {
                return std::unexpected(Located(
                    file, fmt::format("binding names action {} which is not declared in this "
                                      "context's 'Actions'",
                                      FormatHexId(actionId))));
            }
            const ActionKind actionKind = declaration->second;

            // A Button action wants a Whole binding (an X/Y component has no meaning on a digital
            // action); a vector action is driven by component bindings or by a native Whole axis.
            if (actionKind == ActionKind::Button && binding.Axis != AxisComponent::Whole)
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto Button action {} uses axis component '{}'; "
                                      "a Button action takes only 'Whole'",
                                      FormatHexId(actionId), EnumeratorName(binding.Axis))));
            }
            if (actionKind == ActionKind::Axis1D && binding.Axis == AxisComponent::Y)
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto Axis1D action {} uses axis component 'Y'; "
                                      "a 1D action has only an X component",
                                      FormatHexId(actionId))));
            }
            if (!(binding.Threshold >= 0.0f))
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto action {} has a negative 'Threshold' {}",
                                      FormatHexId(actionId), binding.Threshold)));
            }
            if (!(binding.Exponent > 0.0f))
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto action {} has 'Exponent' {}; it must be "
                                      "greater than 0",
                                      FormatHexId(actionId), binding.Exponent)));
            }

            if (binding.Source.Device == InputDeviceType::None)
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto action {} has a 'Source' with Device 'None'; "
                                      "a binding must read a control",
                                      FormatHexId(actionId))));
            }
            if (const string bad = CheckControl(binding.Source); !bad.empty())
            {
                return std::unexpected(
                    Located(file, fmt::format("binding onto action {}: 'Source' {}",
                                              FormatHexId(actionId), bad)));
            }

            // A chord's modifier is read as a button, so it must be a real control, and a
            // digital one must be reachable at its threshold.
            if (const string bad = CheckControl(binding.Modifier); !bad.empty())
            {
                return std::unexpected(
                    Located(file, fmt::format("binding onto action {}: 'Modifier' {}",
                                              FormatHexId(actionId), bad)));
            }
            if (!(binding.ModifierThreshold >= 0.0f))
            {
                return std::unexpected(Located(
                    file,
                    fmt::format("binding onto action {} has a negative 'ModifierThreshold' {}",
                                FormatHexId(actionId), binding.ModifierThreshold)));
            }
            if (IsDigital(binding.Modifier.Device) && binding.ModifierThreshold > 1.0f)
            {
                return std::unexpected(Located(
                    file, fmt::format("binding onto action {} has 'ModifierThreshold' {} on a "
                                      "{} modifier, which reads at most 1 and so is never down",
                                      FormatHexId(actionId), binding.ModifierThreshold,
                                      EnumeratorName(binding.Modifier.Device))));
            }
        }

        // --- 4. Serialize the record + assemble the blob ---

        vector<u8> record;
        WriteFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry);

        CookedInputMapHeader header{};
        header.Version = CookedInputMapVersion;
        header.RecordBytes = static_cast<u32>(record.size());

        vector<u8> blob;
        blob.reserve(sizeof(CookedInputMapHeader) + record.size());
        Append(blob, header);
        blob.insert(blob.end(), record.begin(), record.end());

        return blob;
    }

    void RegisterInputMapImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<InputMapImporter>());
    }
}
