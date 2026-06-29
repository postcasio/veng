#include "MaterialInstanceImporter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

#include "GraphShaderSource.h"
#include "SlangReflect.h"
#include "SlangSession.h"

namespace Veng::Cook
{
    namespace
    {
        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h);
        // truncate rather than fail on an over-long identifier.
        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        // Reads a JSON file into a parsed object, returning a located error on I/O or parse failure.
        Result<json> ReadJsonObject(const path& file, std::string_view label)
        {
            const std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return std::unexpected(fmt::format("material instance importer: failed to open {} "
                                                   "'{}'",
                                                   label, file.string()));
            }
            std::ostringstream stream;
            stream << in.rdbuf();
            const json parsed = json::parse(stream.str(), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                return std::unexpected(fmt::format(
                    "material instance importer: {} '{}': invalid JSON", label, file.string()));
            }
            return parsed;
        }

        // One exposed parent field: its declared authoring type ("vec4"/"texture"/...) paired with
        // the fragment shader's reflected layout, so an override is validated by name (exposure)
        // and by type/offset (the .vmat-against-shader check lifted to instance-against-parent).
        struct ExposedField
        {
            string Type;                 // the parent's declared authoring type
            ReflectedStructField Layout; // the fragment shader's reflected layout for this field
        };

        // Reflects the parent's exposed field set: the parent's own declared "fields" (the exposed
        // override surface — engine-bound fields never appear there), each cross-checked against the
        // parent fragment shader's reflected MaterialParams for its byte offset and type.
        Result<std::map<string, ExposedField>>
        ReflectParentExposedFields(const CookContext& context, const json& parentVmat,
                                   const path& parentVmatPath)
        {
            // Resolve the parent's fragment shader to reflect its MaterialParams layout.
            if (!parentVmat.contains("shaders") || !parentVmat["shaders"].is_object() ||
                !parentVmat["shaders"].contains("fragment") ||
                !parentVmat["shaders"]["fragment"].is_number_unsigned())
            {
                return std::unexpected(fmt::format(
                    "material instance importer: parent '{}' has no 'shaders.fragment' AssetId",
                    parentVmatPath.string()));
            }
            const u64 fragmentShaderId = parentVmat["shaders"]["fragment"].get<u64>();

            const optional<ResolvedSource> fragmentResolved =
                context.Resolve(AssetId{.Value = fragmentShaderId});
            if (!fragmentResolved || fragmentResolved->Type != AssetType::Shader ||
                fragmentResolved->AbsolutePath.empty())
            {
                return std::unexpected(fmt::format(
                    "material instance importer: parent fragment shader {} not resolvable",
                    fragmentShaderId));
            }

            const Result<json> shaderJson =
                ReadJsonObject(fragmentResolved->AbsolutePath, "parent fragment shader json");
            if (!shaderJson)
            {
                return std::unexpected(shaderJson.error());
            }
            if (!shaderJson->contains("source") || !(*shaderJson)["source"].is_string())
            {
                return std::unexpected(fmt::format(
                    "material instance importer: parent fragment shader '{}': missing 'source'",
                    fragmentResolved->AbsolutePath.string()));
            }

            // The fragment source is either a .slang file or a node graph; reflect the generated
            // text in the graph case, exactly as the MaterialImporter does for the parent itself.
            const path shaderJsonDir = fragmentResolved->AbsolutePath.parent_path();
            const Result<GraphShaderSource> graphSource =
                ResolveGraphShaderSourceHook(*shaderJson, shaderJsonDir);
            if (!graphSource)
            {
                return std::unexpected(graphSource.error());
            }

            SlangModuleSource fragSource;
            if (graphSource->IsGraph)
            {
                fragSource = SlangModuleSource{.Path = graphSource->GraphPath,
                                               .GeneratedSource = graphSource->Source};
            }
            else
            {
                fragSource = SlangModuleSource{.Path = shaderJsonDir /
                                                       (*shaderJson)["source"].get<string>()};
            }

            const Result<ReflectedStruct> reflected = ReflectStructLayout(
                fragSource, "MaterialParams", context.ShaderIncludeDir, /*optional=*/true);
            if (!reflected)
            {
                return std::unexpected(reflected.error());
            }

            std::map<string, const ReflectedStructField*> reflectedByName;
            for (const ReflectedStructField& f : reflected->Fields)
            {
                reflectedByName[f.Name] = &f;
            }

            // The parent's declared "fields" are its exposed surface. Each must resolve against the
            // reflected struct (a handle field is a uint member; a param field carries its type).
            if (!parentVmat.contains("fields") || !parentVmat["fields"].is_array())
            {
                return std::unexpected(
                    fmt::format("material instance importer: parent '{}' has no 'fields' array",
                                parentVmatPath.string()));
            }

            std::map<string, ExposedField> exposed;
            for (const json& fieldJson : parentVmat["fields"])
            {
                if (!fieldJson.contains("name") || !fieldJson["name"].is_string() ||
                    !fieldJson.contains("type") || !fieldJson["type"].is_string())
                {
                    continue;
                }
                const string name = fieldJson["name"].get<string>();
                const string type = fieldJson["type"].get<string>();

                // A sampler field is not an override surface — it has no independent value, it
                // mirrors a texture field. Skip it; an override naming it is rejected as non-exposed.
                if (type == "sampler")
                {
                    continue;
                }

                const auto it = reflectedByName.find(name);
                if (it == reflectedByName.end())
                {
                    // A field the parent declares but the shader does not reflect cannot be packed;
                    // the parent cook would already have rejected it, so this is unreachable for a
                    // valid parent. Skip rather than fabricate an override target.
                    continue;
                }
                exposed[name] = ExposedField{.Type = type, .Layout = *it->second};
            }

            return exposed;
        }

        // Assembles CookedMaterialInstanceHeader + override table + value region into one blob.
        vector<u8> BuildBlob(const CookedMaterialInstanceHeader& header,
                             const vector<CookedMaterialInstanceOverride>& overrides,
                             const vector<u8>& valueRegion)
        {
            const usize overrideBytes = overrides.size() * sizeof(CookedMaterialInstanceOverride);
            vector<u8> blob(sizeof(header) + overrideBytes + valueRegion.size());

            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            if (!overrides.empty())
            {
                std::memcpy(blob.data() + cursor, overrides.data(), overrideBytes);
                cursor += overrideBytes;
            }
            if (!valueRegion.empty())
            {
                std::memcpy(blob.data() + cursor, valueRegion.data(), valueRegion.size());
            }
            return blob;
        }
    }

    Result<vector<u8>> MaterialInstanceImporter::Cook(const CookContext& context,
                                                      const json& entry) const
    {
        // --- 1. Read the external *.vmatinst.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("material instance importer: missing or invalid 'source'");
        }

        const path instPath = context.PackDir / entry["source"].get<string>();
        const Result<json> instDocResult = ReadJsonObject(instPath, "instance source");
        if (!instDocResult)
        {
            return std::unexpected(instDocResult.error());
        }
        const json& inst = *instDocResult;

        if (!context.Resolve)
        {
            return std::unexpected("material instance importer: no resolver available");
        }

        // --- 2. Resolve the parent Material ---

        if (!inst.contains("parent") || !inst["parent"].is_number_unsigned())
        {
            return std::unexpected(fmt::format("material instance importer: '{}': 'parent' must be "
                                               "an unsigned integer Material AssetId",
                                               instPath.string()));
        }
        const u64 parentId = inst["parent"].get<u64>();

        const optional<ResolvedSource> parentResolved = context.Resolve(AssetId{.Value = parentId});
        if (!parentResolved)
        {
            return std::unexpected(fmt::format("material instance importer: parent material {} not "
                                               "found in pack or reference packs",
                                               parentId));
        }
        if (parentResolved->Type != AssetType::Material)
        {
            return std::unexpected(
                fmt::format("material instance importer: asset {} referenced as parent but has "
                            "type {} (expected Material)",
                            parentId, static_cast<u32>(parentResolved->Type)));
        }
        if (parentResolved->AbsolutePath.empty())
        {
            return std::unexpected(fmt::format(
                "material instance importer: parent material {} has no resolvable source path",
                parentId));
        }

        const Result<json> parentVmatResult =
            ReadJsonObject(parentResolved->AbsolutePath, "parent material source");
        if (!parentVmatResult)
        {
            return std::unexpected(parentVmatResult.error());
        }

        // --- 3. Reflect the parent's exposed field set (the validated override surface) ---

        const Result<std::map<string, ExposedField>> exposedResult =
            ReflectParentExposedFields(context, *parentVmatResult, parentResolved->AbsolutePath);
        if (!exposedResult)
        {
            return std::unexpected(exposedResult.error());
        }
        const std::map<string, ExposedField>& exposed = *exposedResult;

        // --- 4. Parse + validate the sparse overrides against the exposed surface ---

        // "overrides" is an object: field name → value (param) or { "id": <AssetId> } / <AssetId>
        // (texture). An absent map is a valid zero-override instance.
        vector<CookedMaterialInstanceOverride> overrides;
        vector<u8> valueRegion;

        if (inst.contains("overrides"))
        {
            if (!inst["overrides"].is_object())
            {
                return std::unexpected(
                    fmt::format("material instance importer: '{}': 'overrides' must be an object",
                                instPath.string()));
            }

            for (const auto& [name, value] : inst["overrides"].items())
            {
                const auto exposedIt = exposed.find(name);
                if (exposedIt == exposed.end())
                {
                    return std::unexpected(fmt::format("material instance importer: '{}': override "
                                                       "'{}' is not an exposed field of "
                                                       "parent material {} (an engine-bound or "
                                                       "undeclared field is not overridable)",
                                                       instPath.string(), name, parentId));
                }
                const ExposedField& field = exposedIt->second;

                CookedMaterialInstanceOverride co{};
                SetName(co.Name, name);

                if (field.Type == "texture")
                {
                    // A texture override carries an AssetId — accept either a bare unsigned id or
                    // an { "id": <AssetId> } object, mirroring the .vmat texture-field spelling.
                    u64 textureId = 0;
                    if (value.is_number_unsigned())
                    {
                        textureId = value.get<u64>();
                    }
                    else if (value.is_object() && value.contains("id") &&
                             value["id"].is_number_unsigned())
                    {
                        textureId = value["id"].get<u64>();
                    }
                    else
                    {
                        return std::unexpected(fmt::format(
                            "material instance importer: '{}': texture override '{}' must be an "
                            "unsigned AssetId (or {{ \"id\": <AssetId> }})",
                            instPath.string(), name));
                    }

                    const optional<ResolvedSource> texResolved =
                        context.Resolve(AssetId{.Value = textureId});
                    if (!texResolved)
                    {
                        return std::unexpected(fmt::format(
                            "material instance importer: '{}': override texture {} for field '{}' "
                            "not found in pack or reference packs",
                            instPath.string(), textureId, name));
                    }
                    if (texResolved->Type != AssetType::Texture)
                    {
                        return std::unexpected(fmt::format(
                            "material instance importer: '{}': asset {} for field '{}' has type {} "
                            "(expected Texture)",
                            instPath.string(), textureId, name,
                            static_cast<u32>(texResolved->Type)));
                    }

                    co.Kind = 1; // texture
                    co.ValueOffset = 0;
                    co.ValueSize = 0;
                    co.TextureId = textureId;
                }
                else
                {
                    // A param override (float / vecN / uint) — validate its arity against the
                    // declared type and the reflected scalar/vector type, then pack its bytes.
                    const ReflectedStructField& layout = field.Layout;

                    if (field.Type == "uint")
                    {
                        if (layout.IsFloat || layout.ComponentCount != 1)
                        {
                            return std::unexpected(fmt::format(
                                "material instance importer: '{}': override '{}' is declared uint "
                                "but the parent field is not a scalar uint",
                                instPath.string(), name));
                        }
                        if (!value.is_number_unsigned())
                        {
                            return std::unexpected(fmt::format(
                                "material instance importer: '{}': uint override '{}' must be an "
                                "unsigned integer",
                                instPath.string(), name));
                        }
                        const u32 v = value.get<u32>();
                        co.Kind = 0;
                        co.ValueOffset = static_cast<u32>(valueRegion.size());
                        co.ValueSize = sizeof(u32);
                        co.TextureId = 0;
                        const auto* bytes = reinterpret_cast<const u8*>(&v);
                        valueRegion.insert(valueRegion.end(), bytes, bytes + sizeof(u32));
                    }
                    else
                    {
                        const u32 arity = field.Type == "float"  ? 1u
                                          : field.Type == "vec2" ? 2u
                                          : field.Type == "vec3" ? 3u
                                          : field.Type == "vec4" ? 4u
                                                                 : 0u;
                        if (arity == 0)
                        {
                            return std::unexpected(fmt::format(
                                "material instance importer: '{}': override '{}' has unsupported "
                                "parent field type '{}'",
                                instPath.string(), name, field.Type));
                        }
                        if (!layout.IsFloat || layout.ComponentCount != arity)
                        {
                            return std::unexpected(
                                fmt::format("material instance importer: '{}': override '{}' is "
                                            "declared {} but "
                                            "the parent field's reflected layout does not match",
                                            instPath.string(), name, field.Type));
                        }

                        vector<f32> values;
                        if (arity == 1)
                        {
                            if (!value.is_number())
                            {
                                return std::unexpected(fmt::format(
                                    "material instance importer: '{}': float override '{}' must be "
                                    "a number",
                                    instPath.string(), name));
                            }
                            values.push_back(value.get<f32>());
                        }
                        else
                        {
                            if (!value.is_array() || value.size() != arity)
                            {
                                return std::unexpected(fmt::format(
                                    "material instance importer: '{}': {} override '{}' must be an "
                                    "array of {} numbers",
                                    instPath.string(), field.Type, name, arity));
                            }
                            for (const json& elem : value)
                            {
                                if (!elem.is_number())
                                {
                                    return std::unexpected(fmt::format(
                                        "material instance importer: '{}': override '{}' array "
                                        "contains a non-number element",
                                        instPath.string(), name));
                                }
                                values.push_back(elem.get<f32>());
                            }
                        }

                        co.Kind = 0;
                        co.ValueOffset = static_cast<u32>(valueRegion.size());
                        co.ValueSize = static_cast<u32>(values.size() * sizeof(f32));
                        co.TextureId = 0;
                        const auto* bytes = reinterpret_cast<const u8*>(values.data());
                        valueRegion.insert(valueRegion.end(), bytes, bytes + co.ValueSize);
                    }
                }

                overrides.push_back(co);
            }
        }

        // --- 5. Assemble the blob ---

        CookedMaterialInstanceHeader header{};
        header.ParentId = parentId;
        header.Version = CookedMaterialInstanceVersion;
        header.OverrideCount = static_cast<u32>(overrides.size());
        header.ValueRegionBytes = static_cast<u32>(valueRegion.size());

        return BuildBlob(header, overrides, valueRegion);
    }
}
