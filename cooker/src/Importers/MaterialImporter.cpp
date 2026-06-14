#include "MaterialImporter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

#include "SlangReflect.h"

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

        // Assembles the final cooked material blob:
        //   CookedMaterialHeader
        //   CookedMaterialField[FieldCount]
        //   packed param block (ParamBytes)
        vector<u8> BuildBlob(const CookedMaterialHeader& header,
            const vector<CookedMaterialField>& fields,
            const vector<u8>& paramBlock)
        {
            const usize fieldBytes = fields.size() * sizeof(CookedMaterialField);
            vector<u8> blob(sizeof(CookedMaterialHeader) + fieldBytes + paramBlock.size());

            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            if (!fields.empty())
            {
                std::memcpy(blob.data() + cursor, fields.data(), fieldBytes);
                cursor += fieldBytes;
            }
            if (!paramBlock.empty())
            {
                std::memcpy(blob.data() + cursor, paramBlock.data(), paramBlock.size());
            }

            return blob;
        }
    }

    Result<vector<u8>> MaterialImporter::Cook(const CookContext& context, const json& entry) const
    {
        // --- 1. Read the external *.vmat.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("material importer: missing or invalid 'source'");

        const path vmatPath = context.PackDir / entry["source"].get<string>();

        std::ifstream vmatFile(vmatPath, std::ios::binary);
        if (!vmatFile)
        {
            return std::unexpected(
                fmt::format("material importer: failed to open '{}'", vmatPath.string()));
        }

        std::ostringstream vmatStream;
        vmatStream << vmatFile.rdbuf();
        const json vmat = json::parse(vmatStream.str(), nullptr, false);
        if (vmat.is_discarded() || !vmat.is_object())
        {
            return std::unexpected(
                fmt::format("material importer: '{}': invalid JSON", vmatPath.string()));
        }

        // --- 2. Validate and resolve shader references ---

        if (!vmat.contains("shaders") || !vmat["shaders"].is_object())
            return std::unexpected("material importer: missing or invalid 'shaders' object");

        const json& shaders = vmat["shaders"];

        if (!shaders.contains("vertex") || !shaders["vertex"].is_number_unsigned())
            return std::unexpected("material importer: 'shaders.vertex' must be an unsigned integer AssetId");

        if (!shaders.contains("fragment") || !shaders["fragment"].is_number_unsigned())
            return std::unexpected("material importer: 'shaders.fragment' must be an unsigned integer AssetId");

        const u64 vertexShaderId = shaders["vertex"].get<u64>();
        const u64 fragmentShaderId = shaders["fragment"].get<u64>();

        if (!context.Resolve)
            return std::unexpected("material importer: no resolver available");

        const optional<ResolvedSource> vertexResolved = context.Resolve(AssetId{.Value = vertexShaderId});
        if (!vertexResolved)
        {
            return std::unexpected(fmt::format(
                "material importer: vertex shader {} not found in pack or reference packs",
                vertexShaderId));
        }
        if (vertexResolved->Type != AssetType::Shader)
        {
            return std::unexpected(fmt::format(
                "material importer: asset {} referenced as vertex shader but has type {}",
                vertexShaderId, static_cast<u32>(vertexResolved->Type)));
        }

        const optional<ResolvedSource> fragmentResolved = context.Resolve(AssetId{.Value = fragmentShaderId});
        if (!fragmentResolved)
        {
            return std::unexpected(fmt::format(
                "material importer: fragment shader {} not found in pack or reference packs",
                fragmentShaderId));
        }
        if (fragmentResolved->Type != AssetType::Shader)
        {
            return std::unexpected(fmt::format(
                "material importer: asset {} referenced as fragment shader but has type {}",
                fragmentShaderId, static_cast<u32>(fragmentResolved->Type)));
        }

        // The fragment shader supplies the MaterialData layout, so it must resolve
        // to a source path the cook can reflect.
        if (fragmentResolved->AbsolutePath.empty())
        {
            return std::unexpected(fmt::format(
                "material importer: fragment shader {} has no resolvable source path",
                fragmentShaderId));
        }

        // --- 3. Reflect MaterialData from the fragment shader's Slang source ---

        // The fragment shader's AbsolutePath points at its *.shader.json, not a .slang.
        // Read that JSON to locate the actual .slang source relative to its directory.
        const path shaderJsonPath = fragmentResolved->AbsolutePath;

        std::ifstream shaderJsonFile(shaderJsonPath, std::ios::binary);
        if (!shaderJsonFile)
        {
            return std::unexpected(fmt::format(
                "material importer: failed to open fragment shader json '{}'",
                shaderJsonPath.string()));
        }

        std::ostringstream shaderJsonStream;
        shaderJsonStream << shaderJsonFile.rdbuf();
        const json shaderJson = json::parse(shaderJsonStream.str(), nullptr, false);
        if (shaderJson.is_discarded() || !shaderJson.is_object())
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': invalid JSON", shaderJsonPath.string()));
        }

        if (!shaderJson.contains("source") || !shaderJson["source"].is_string())
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': missing or invalid 'source'", shaderJsonPath.string()));
        }

        const path fragSlangPath =
            shaderJsonPath.parent_path() / shaderJson["source"].get<string>();

        const Result<ReflectedStruct> reflected =
            ReflectStructLayout(fragSlangPath, "MaterialData");
        if (!reflected)
            return std::unexpected(reflected.error());

        // --- 4. Parse vmat["fields"] into an ordered declared-field list ---

        if (!vmat.contains("fields") || !vmat["fields"].is_array())
            return std::unexpected("material importer: missing or invalid 'fields' array");

        struct DeclaredField
        {
            string Name;
            string Type;
            // texture / sampler
            u64 TextureAssetId = 0;       // texture: the asset id
            string SamplerTextureName;    // sampler: the name of the referenced texture field
            // float / vecN / uint
            vector<f32> FloatValues;
            u32 UintValue = 0;
            // bookkeeping
            bool Matched = false;
        };

        vector<DeclaredField> declaredFields;
        // name → index in declaredFields, for duplicate detection and sampler lookup
        std::map<string, usize> declaredByName;

        const json& fieldsJson = vmat["fields"];

        for (const json& fieldJson : fieldsJson)
        {
            if (!fieldJson.contains("name") || !fieldJson["name"].is_string())
                return std::unexpected("material importer: field entry missing or invalid 'name'");
            if (!fieldJson.contains("type") || !fieldJson["type"].is_string())
                return std::unexpected("material importer: field entry missing or invalid 'type'");

            DeclaredField decl;
            decl.Name = fieldJson["name"].get<string>();
            decl.Type = fieldJson["type"].get<string>();

            if (declaredByName.count(decl.Name))
            {
                return std::unexpected(fmt::format(
                    "material importer: duplicate declared field '{}'", decl.Name));
            }

            const vector<string> AllowedTypes = {
                "texture", "sampler", "float", "vec2", "vec3", "vec4", "uint"
            };
            if (std::find(AllowedTypes.begin(), AllowedTypes.end(), decl.Type) == AllowedTypes.end())
            {
                return std::unexpected(fmt::format(
                    "material importer: field '{}' has unknown type '{}'", decl.Name, decl.Type));
            }

            if (decl.Type == "texture")
            {
                if (!fieldJson.contains("id") || !fieldJson["id"].is_number_unsigned())
                {
                    return std::unexpected(fmt::format(
                        "material importer: texture field '{}' must have an unsigned integer 'id'",
                        decl.Name));
                }
                decl.TextureAssetId = fieldJson["id"].get<u64>();
            }
            else if (decl.Type == "sampler")
            {
                if (!fieldJson.contains("texture") || !fieldJson["texture"].is_string())
                {
                    return std::unexpected(fmt::format(
                        "material importer: sampler field '{}' must have a 'texture' name",
                        decl.Name));
                }
                decl.SamplerTextureName = fieldJson["texture"].get<string>();
            }
            else if (decl.Type == "uint")
            {
                if (!fieldJson.contains("value") || !fieldJson["value"].is_number_unsigned())
                {
                    return std::unexpected(fmt::format(
                        "material importer: uint field '{}' must have an unsigned integer 'value'",
                        decl.Name));
                }
                decl.UintValue = fieldJson["value"].get<u32>();
            }
            else
            {
                // float / vec2 / vec3 / vec4
                const u32 expectedArity =
                    decl.Type == "float" ? 1u :
                    decl.Type == "vec2"  ? 2u :
                    decl.Type == "vec3"  ? 3u : 4u;

                if (!fieldJson.contains("value"))
                {
                    return std::unexpected(fmt::format(
                        "material importer: field '{}' of type '{}' must have a 'value'",
                        decl.Name, decl.Type));
                }

                const json& val = fieldJson["value"];
                if (expectedArity == 1)
                {
                    if (!val.is_number())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: float field '{}' 'value' must be a number",
                            decl.Name));
                    }
                    decl.FloatValues.push_back(val.get<f32>());
                }
                else
                {
                    if (!val.is_array() || val.size() != expectedArity)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: {} field '{}' 'value' must be an array of {} numbers",
                            decl.Type, decl.Name, expectedArity));
                    }
                    for (const json& elem : val)
                    {
                        if (!elem.is_number())
                        {
                            return std::unexpected(fmt::format(
                                "material importer: {} field '{}' 'value' array contains a non-number element",
                                decl.Type, decl.Name));
                        }
                        decl.FloatValues.push_back(elem.get<f32>());
                    }
                }
            }

            const usize idx = declaredFields.size();
            declaredFields.push_back(std::move(decl));
            declaredByName[declaredFields[idx].Name] = idx;
        }

        // --- 5. Build the zero-initialized param block ---

        vector<u8> paramBlock(reflected->Size, 0);

        // --- 6. Walk reflected fields in order, producing one CookedMaterialField each ---

        vector<CookedMaterialField> fields;
        fields.reserve(reflected->Fields.size());

        for (const ReflectedStructField& reflField : reflected->Fields)
        {
            CookedMaterialField cookedField{};
            SetName(cookedField.Name, reflField.Name);
            cookedField.Offset = reflField.Offset;
            cookedField.Size = reflField.Size;
            cookedField.Kind = 0;
            cookedField.TextureId = 0;

            auto declIt = declaredByName.find(reflField.Name);
            if (declIt != declaredByName.end())
            {
                DeclaredField& decl = declaredFields[declIt->second];
                decl.Matched = true;

                if (decl.Type == "texture")
                {
                    // Must be a scalar uint handle slot.
                    if (reflField.IsFloat || reflField.ComponentCount != 1)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: texture field '{}' maps to a reflected field that is "
                            "not a scalar uint (IsFloat={}, ComponentCount={})",
                            decl.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount));
                    }

                    const optional<ResolvedSource> texResolved =
                        context.Resolve(AssetId{.Value = decl.TextureAssetId});
                    if (!texResolved)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: texture {} for field '{}' not found in pack or reference packs",
                            decl.TextureAssetId, decl.Name));
                    }
                    if (texResolved->Type != AssetType::Texture)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: asset {} referenced as texture for field '{}' but has type {}",
                            decl.TextureAssetId, decl.Name,
                            static_cast<u32>(texResolved->Type)));
                    }

                    cookedField.Kind = 1;
                    cookedField.TextureId = decl.TextureAssetId;
                }
                else if (decl.Type == "sampler")
                {
                    // Must be a scalar uint handle slot.
                    if (reflField.IsFloat || reflField.ComponentCount != 1)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: sampler field '{}' maps to a reflected field that is "
                            "not a scalar uint (IsFloat={}, ComponentCount={})",
                            decl.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount));
                    }

                    // Look up the referenced texture field by name among declared fields.
                    auto texDeclIt = declaredByName.find(decl.SamplerTextureName);
                    if (texDeclIt == declaredByName.end() ||
                        declaredFields[texDeclIt->second].Type != "texture")
                    {
                        return std::unexpected(fmt::format(
                            "material importer: sampler field '{}' references '{}' which is not a "
                            "declared texture field",
                            decl.Name, decl.SamplerTextureName));
                    }

                    cookedField.Kind = 2;
                    cookedField.TextureId = declaredFields[texDeclIt->second].TextureAssetId;
                }
                else if (decl.Type == "uint")
                {
                    // Must be a scalar uint.
                    if (reflField.IsFloat || reflField.ComponentCount != 1)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: uint field '{}' maps to a reflected field that is "
                            "not a scalar uint (IsFloat={}, ComponentCount={})",
                            decl.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount));
                    }

                    const usize writeEnd =
                        static_cast<usize>(reflField.Offset) + sizeof(u32);
                    if (writeEnd > paramBlock.size())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: uint field '{}' at offset {} + 4 bytes "
                            "exceeds param block size {}",
                            decl.Name, reflField.Offset, paramBlock.size()));
                    }

                    std::memcpy(paramBlock.data() + reflField.Offset, &decl.UintValue, sizeof(u32));
                    cookedField.Kind = 0;
                    cookedField.TextureId = 0;
                }
                else
                {
                    // float / vec2 / vec3 / vec4
                    const u32 expectedComponents =
                        decl.Type == "float" ? 1u :
                        decl.Type == "vec2"  ? 2u :
                        decl.Type == "vec3"  ? 3u : 4u;

                    if (!reflField.IsFloat || reflField.ComponentCount != expectedComponents)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: {} field '{}' maps to a reflected field with "
                            "IsFloat={}, ComponentCount={} (expected IsFloat=true, ComponentCount={})",
                            decl.Type, decl.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount,
                            expectedComponents));
                    }

                    const usize writeEnd =
                        static_cast<usize>(reflField.Offset) +
                        decl.FloatValues.size() * sizeof(f32);
                    if (writeEnd > paramBlock.size())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: {} field '{}' at offset {} + {} bytes "
                            "exceeds param block size {}",
                            decl.Type, decl.Name,
                            reflField.Offset,
                            decl.FloatValues.size() * sizeof(f32),
                            paramBlock.size()));
                    }

                    // Write as little-endian f32 (host order == LE on macOS/x86).
                    std::memcpy(paramBlock.data() + reflField.Offset,
                        decl.FloatValues.data(),
                        decl.FloatValues.size() * sizeof(f32));

                    cookedField.Kind = 0;
                    cookedField.TextureId = 0;
                }
            }
            // No declared field matched this reflected field (e.g. a pad): leave Kind=0, TextureId=0.

            fields.push_back(cookedField);
        }

        // --- 7. Every declared field must match a reflected field ---

        for (const DeclaredField& decl : declaredFields)
        {
            if (!decl.Matched)
            {
                return std::unexpected(fmt::format(
                    "material importer: field '{}' does not match any field in MaterialData",
                    decl.Name));
            }
        }

        // --- 8. Assemble the blob ---

        CookedMaterialHeader header{};
        header.VertexShaderId = vertexShaderId;
        header.FragmentShaderId = fragmentShaderId;
        header.FieldCount = static_cast<u32>(fields.size());
        header.ParamBytes = reflected->Size;

        return BuildBlob(header, fields, paramBlock);
    }
}
