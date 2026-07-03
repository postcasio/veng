#include "InputMapImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Input/Actions.h>
#include <Veng/Reflection/EnumName.h>
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
        // it needs no game module: build a builtin-only registry for the enum tables + WriteFields.
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);

        // --- 2. Actions ---

        InputMapData data;

        if (!doc.contains("actions") || !doc["actions"].is_array())
        {
            return std::unexpected(Located(file, "missing or invalid 'actions' array"));
        }

        // Track declared action ids for the binding cross-check and for the uniqueness rule.
        map<u64, ActionKind> declared;
        for (const json& actionJson : doc["actions"])
        {
            if (!actionJson.is_object())
            {
                return std::unexpected(Located(file, "each 'actions' entry must be an object"));
            }
            if (!actionJson.contains("id") || !actionJson["id"].is_string())
            {
                return std::unexpected(Located(file, "an action is missing a hex-id-string 'id'"));
            }

            const optional<u64> parsedId = ParseHexId(actionJson["id"].get<string>());
            if (!parsedId)
            {
                return std::unexpected(
                    Located(file, fmt::format("an action 'id' is a malformed hex id '{}'",
                                              actionJson["id"].get<string>())));
            }

            const u64 id = *parsedId;
            if (id == 0)
            {
                return std::unexpected(Located(file, "an action 'id' must be non-null"));
            }
            if (declared.contains(id))
            {
                return std::unexpected(
                    Located(file, fmt::format("action id {} is declared more than once", id)));
            }

            const string kindName =
                actionJson.contains("kind") ? actionJson["kind"].get<string>() : string("Button");
            const optional<ActionKind> kind = ParseEnum<ActionKind>(kindName);
            if (!kind)
            {
                return std::unexpected(Located(
                    file, fmt::format("action {}: kind: unknown value '{}'", id, kindName)));
            }

            InputAction action;
            action.Id = static_cast<ActionId>(id);
            action.Name = actionJson.contains("name") ? actionJson["name"].get<string>() : string();
            action.Kind = *kind;

            declared.emplace(id, *kind);
            data.Actions.push_back(std::move(action));
        }

        // --- 3. Bindings ---

        if (doc.contains("bindings"))
        {
            if (!doc["bindings"].is_array())
            {
                return std::unexpected(Located(file, "'bindings' must be an array"));
            }

            for (const json& bindingJson : doc["bindings"])
            {
                if (!bindingJson.is_object())
                {
                    return std::unexpected(
                        Located(file, "each 'bindings' entry must be an object"));
                }

                if (!bindingJson.contains("action") || !bindingJson["action"].is_string())
                {
                    return std::unexpected(
                        Located(file, "a binding is missing a hex-id-string 'action' id"));
                }
                const optional<u64> parsedAction = ParseHexId(bindingJson["action"].get<string>());
                if (!parsedAction)
                {
                    return std::unexpected(
                        Located(file, fmt::format("a binding 'action' is a malformed hex id '{}'",
                                                  bindingJson["action"].get<string>())));
                }
                const u64 actionId = *parsedAction;

                // The typo-catch a global registry would otherwise miss: a binding must name an
                // action this context declares.
                const auto declaration = declared.find(actionId);
                if (declaration == declared.end())
                {
                    return std::unexpected(Located(
                        file, fmt::format("binding names action {} which is not declared in this "
                                          "context's 'actions'",
                                          actionId)));
                }
                const ActionKind actionKind = declaration->second;

                // Source device + control.
                if (!bindingJson.contains("source") || !bindingJson["source"].is_object())
                {
                    return std::unexpected(Located(file, "a binding is missing a 'source' object"));
                }
                const json& sourceJson = bindingJson["source"];

                const string deviceName = sourceJson.contains("device")
                                              ? sourceJson["device"].get<string>()
                                              : string("Keyboard");
                const optional<InputDeviceType> device = ParseEnum<InputDeviceType>(deviceName);
                if (!device)
                {
                    return std::unexpected(
                        Located(file, fmt::format("binding source: device: unknown value '{}'",
                                                  deviceName)));
                }

                if (!sourceJson.contains("control") || !sourceJson["control"].is_number_unsigned())
                {
                    return std::unexpected(
                        Located(file, "a binding source is missing an unsigned 'control' code"));
                }

                // Axis component (defaults to Whole).
                const string axisName = bindingJson.contains("axis")
                                            ? bindingJson["axis"].get<string>()
                                            : string("Whole");
                const optional<AxisComponent> axis = ParseEnum<AxisComponent>(axisName);
                if (!axis)
                {
                    return std::unexpected(
                        Located(file, fmt::format("binding axis: unknown value '{}'", axisName)));
                }

                // Axis/kind consistency: a Button action wants a Whole binding (an X/Y component
                // has no meaning on a digital action); a vector action (Axis1D/Axis2D) is driven
                // by component bindings or by a native Whole axis, so both are allowed there.
                if (actionKind == ActionKind::Button && *axis != AxisComponent::Whole)
                {
                    return std::unexpected(Located(
                        file, fmt::format("binding onto Button action {} uses axis component '{}'; "
                                          "a Button action takes only 'Whole'",
                                          actionId, axisName)));
                }
                // A Y component on a 1D action has no target component.
                if (actionKind == ActionKind::Axis1D && *axis == AxisComponent::Y)
                {
                    return std::unexpected(Located(
                        file, fmt::format("binding onto Axis1D action {} uses axis component 'Y'; "
                                          "a 1D action has only an X component",
                                          actionId)));
                }

                Binding binding;
                binding.Source.Device = *device;
                binding.Source.Control = sourceJson["control"].get<u32>();
                binding.Action = static_cast<ActionId>(actionId);
                binding.Axis = *axis;
                binding.Scale =
                    bindingJson.contains("scale") ? bindingJson["scale"].get<f32>() : 1.0f;

                data.Bindings.push_back(binding);
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
