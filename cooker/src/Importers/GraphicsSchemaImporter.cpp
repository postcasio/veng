#include "GraphicsSchemaImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Scene/BuiltinTypes.h>

namespace Veng::Cook
{
    namespace
    {
        // Located-error prefix for a graphics-schema field.
        string Located(const string& file, const string& reason)
        {
            return fmt::format("graphics schema importer: '{}': {}", file, reason);
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }

        // Validates the decoded schema. Every failure is a located error naming the offending id,
        // matching the LevelImporter's diagnostics.
        VoidResult Validate(const GraphicsSchemaData& schema, const string& file)
        {
            // Every schema setting id, for the preset cross-check, and each setting's option set.
            std::unordered_set<string> settingIds;
            std::unordered_map<string, const GraphicsSetting*> settingsById;
            std::unordered_set<string> categoryIds;

            for (const GraphicsCategory& category : schema.Categories)
            {
                if (category.Id.empty())
                {
                    return std::unexpected(Located(file, "a category has an empty 'Id'"));
                }
                if (!categoryIds.insert(category.Id).second)
                {
                    return std::unexpected(
                        Located(file, fmt::format("category id '{}' is declared more than once",
                                                  category.Id)));
                }

                for (const GraphicsSetting& setting : category.Settings)
                {
                    if (setting.Id.empty())
                    {
                        return std::unexpected(Located(
                            file, fmt::format("category '{}' has a setting with an empty 'Id'",
                                              category.Id)));
                    }
                    if (setting.Id == GraphicsRenderScaleBuiltinId)
                    {
                        return std::unexpected(Located(
                            file, fmt::format("setting id '{}' is reserved for the render-scale "
                                              "built-in and cannot be a schema setting",
                                              setting.Id)));
                    }
                    if (!settingIds.insert(setting.Id).second)
                    {
                        return std::unexpected(
                            Located(file, fmt::format("setting id '{}' is declared more than once",
                                                      setting.Id)));
                    }
                    settingsById.emplace(setting.Id, &setting);

                    if (setting.Kind == GraphicsSettingKind::Discrete)
                    {
                        if (setting.Options.empty())
                        {
                            return std::unexpected(Located(
                                file, fmt::format("discrete setting '{}' declares no options",
                                                  setting.Id)));
                        }
                        std::unordered_set<string> optionIds;
                        for (const GraphicsOption& option : setting.Options)
                        {
                            if (option.Id.empty())
                            {
                                return std::unexpected(Located(
                                    file,
                                    fmt::format("setting '{}' has an option with an empty 'Id'",
                                                setting.Id)));
                            }
                            if (!optionIds.insert(option.Id).second)
                            {
                                return std::unexpected(Located(
                                    file, fmt::format("setting '{}' declares option id '{}' more "
                                                      "than once",
                                                      setting.Id, option.Id)));
                            }
                        }
                        if (setting.DefaultOption >= setting.Options.size())
                        {
                            return std::unexpected(Located(
                                file,
                                fmt::format("discrete setting '{}' has DefaultOption {} out of "
                                            "range for its {} option(s)",
                                            setting.Id, setting.DefaultOption,
                                            setting.Options.size())));
                        }
                    }
                    else // GraphicsSettingKind::Scalar
                    {
                        if (!(setting.Min < setting.Max))
                        {
                            return std::unexpected(Located(
                                file, fmt::format("scalar setting '{}' needs Min < Max (Min {}, "
                                                  "Max {})",
                                                  setting.Id, setting.Min, setting.Max)));
                        }
                        if (setting.DefaultValue < setting.Min ||
                            setting.DefaultValue > setting.Max)
                        {
                            return std::unexpected(Located(
                                file, fmt::format("scalar setting '{}' has DefaultValue {} outside "
                                                  "[{}, {}]",
                                                  setting.Id, setting.DefaultValue, setting.Min,
                                                  setting.Max)));
                        }
                    }
                }
            }

            // Presets: unique ids, and every entry names a real target.
            std::unordered_set<string> presetIds;
            for (const GraphicsPreset& preset : schema.Presets)
            {
                if (preset.Id.empty())
                {
                    return std::unexpected(Located(file, "a preset has an empty 'Id'"));
                }
                if (!presetIds.insert(preset.Id).second)
                {
                    return std::unexpected(Located(
                        file, fmt::format("preset id '{}' is declared more than once", preset.Id)));
                }

                for (const GraphicsPresetEntry& presetEntry : preset.Entries)
                {
                    if (presetEntry.SettingId == GraphicsRenderScaleBuiltinId)
                    {
                        // The one preset-eligible built-in; it carries a scalar value, no option.
                        continue;
                    }

                    const auto found = settingsById.find(presetEntry.SettingId);
                    if (found == settingsById.end())
                    {
                        return std::unexpected(Located(
                            file, fmt::format("preset '{}' names setting '{}', which is neither a "
                                              "schema setting nor the '{}' built-in",
                                              preset.Id, presetEntry.SettingId,
                                              GraphicsRenderScaleBuiltinId)));
                    }

                    const GraphicsSetting& setting = *found->second;
                    if (setting.Kind == GraphicsSettingKind::Discrete)
                    {
                        const bool known =
                            std::ranges::any_of(setting.Options, [&](const GraphicsOption& option)
                                                { return option.Id == presetEntry.OptionId; });
                        if (!known)
                        {
                            return std::unexpected(Located(
                                file, fmt::format(
                                          "preset '{}' sets discrete setting '{}' to option "
                                          "'{}', which it does not declare",
                                          preset.Id, presetEntry.SettingId, presetEntry.OptionId)));
                        }
                    }
                }
            }

            if (!schema.DefaultPreset.empty() && !presetIds.contains(schema.DefaultPreset))
            {
                return std::unexpected(
                    Located(file, fmt::format("DefaultPreset '{}' names no declared preset",
                                              schema.DefaultPreset)));
            }

            return {};
        }
    }

    Result<vector<u8>> GraphicsSchemaImporter::Cook(const CookContext& context,
                                                    const json& entry) const
    {
        // --- 1. Read + parse the external *.gfxschema.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("graphics schema importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string file = sourcePath.string();

        const Result<json> docResult = ReadJsonFile(sourcePath, "graphics schema importer");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }
        const json& doc = *docResult;

        if (!doc.is_object())
        {
            return std::unexpected(Located(file, "the schema source must be a JSON object"));
        }

        // The schema references only engine builtins (its own reflected structs), so it needs no
        // game module: a builtin-only registry supplies the enum tables + the WriteFields encoder.
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);

        // --- 2. Bind the whole schema through the shared walker (strict: unknown key → error) ---

        const TypeInfo& type = registry.Info(TypeIdOf<GraphicsSchemaData>());
        GraphicsSchemaData schema;
        const VoidResult bound = JsonReadFields(&schema, type, doc, registry);
        if (!bound)
        {
            return std::unexpected(Located(file, bound.error()));
        }

        // --- 3. Validate ---

        const VoidResult valid = Validate(schema, file);
        if (!valid)
        {
            return std::unexpected(valid.error());
        }

        // --- 4. Serialize the record + assemble the blob ---

        vector<u8> record;
        WriteFields(record, &schema, type, registry);

        CookedGraphicsSchemaHeader header{};
        header.Version = CookedGraphicsSchemaVersion;
        header.RecordBytes = static_cast<u32>(record.size());

        vector<u8> blob;
        blob.reserve(sizeof(CookedGraphicsSchemaHeader) + record.size());
        Append(blob, header);
        blob.insert(blob.end(), record.begin(), record.end());

        return blob;
    }

    void RegisterGraphicsSchemaImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<GraphicsSchemaImporter>());
    }
}
