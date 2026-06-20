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
        // The per-material block buffer stride. Mirrors
        // Renderer::BindlessRegistry::MaterialParamStride; restated here so the
        // cooker gains no renderer-header dependency.
        constexpr u32 MaterialParamStride = 256;

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h);
        // truncate rather than fail on an over-long identifier.
        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        // Assembles CookedMaterialHeader + CookedMaterialField[] + param block into one blob.
        vector<u8> BuildBlob(const CookedMaterialHeader& header,
            const vector<CookedMaterialField>& fields,
            const vector<u8>& block)
        {
            const usize fieldBytes = fields.size() * sizeof(CookedMaterialField);
            vector<u8> blob(sizeof(CookedMaterialHeader) + fieldBytes + block.size());

            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            if (!fields.empty())
            {
                std::memcpy(blob.data() + cursor, fields.data(), fieldBytes);
                cursor += fieldBytes;
            }
            if (!block.empty())
            {
                std::memcpy(blob.data() + cursor, block.data(), block.size());
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

        // --- 1b. Parse the optional domain (default surface) ---

        // Absent → Surface (0); unknown value is a located cook error.
        u32 domain = 0; // MaterialDomain::Surface
        if (vmat.contains("domain"))
        {
            if (!vmat["domain"].is_string())
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': 'domain' must be a string (\"surface\" or \"postprocess\")",
                    vmatPath.string()));
            }
            const string domainStr = vmat["domain"].get<string>();
            if (domainStr == "surface")
                domain = 0;
            else if (domainStr == "postprocess")
                domain = 1;
            else
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': unknown domain '{}' (expected \"surface\" or \"postprocess\")",
                    vmatPath.string(), domainStr));
            }
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

        // The fragment shader supplies the param-block layout, so it must resolve
        // to a source path the cook can reflect.
        if (fragmentResolved->AbsolutePath.empty())
        {
            return std::unexpected(fmt::format(
                "material importer: fragment shader {} has no resolvable source path",
                fragmentShaderId));
        }

        // --- 3. Reflect the param block from the fragment shader's Slang source ---

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

        if (!shaderJson.contains("entry") || !shaderJson["entry"].is_string())
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': missing or invalid 'entry'", shaderJsonPath.string()));
        }
        const string fragEntry = shaderJson["entry"].get<string>();

        // --- 3b. Validate the fragment outputs against the domain's contract ---

        // Surface: float4 SV_Target0 (albedo) + SV_Target1 (normal) + SV_Target2 (ORM).
        // PostProcess: single float4 SV_Target0. Mismatch is a located cook error.
        const Result<vector<ReflectedFragmentOutput>> outputs =
            ReflectFragmentOutputs(fragSlangPath, fragEntry);
        if (!outputs)
            return std::unexpected(outputs.error());

        if (domain == 0) // Surface
        {
            const bool ok = outputs->size() == 3
                && (*outputs)[0].TargetIndex == 0 && (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4
                && (*outputs)[1].TargetIndex == 1 && (*outputs)[1].IsFloat && (*outputs)[1].ComponentCount == 4
                && (*outputs)[2].TargetIndex == 2 && (*outputs)[2].IsFloat && (*outputs)[2].ComponentCount == 4;
            if (!ok)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': surface material must write the g-buffer "
                    "(float4 SV_Target0 + float4 SV_Target1 + float4 SV_Target2); "
                    "its fragment shader does not",
                    vmatPath.string()));
            }
        }
        else // PostProcess
        {
            const bool ok = outputs->size() == 1
                && (*outputs)[0].TargetIndex == 0 && (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4;
            if (!ok)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': postprocess material must write a single "
                    "float4 SV_Target0 and no further targets; its fragment shader does not",
                    vmatPath.string()));
            }
        }

        // MaterialParams is optional — a fieldless material declares no struct,
        // which reflects as an empty (Size 0) layout.
        const Result<ReflectedStruct> blockReflected =
            ReflectStructLayout(fragSlangPath, "MaterialParams", /*optional=*/true);
        if (!blockReflected)
            return std::unexpected(blockReflected.error());

        if (blockReflected->Size > MaterialParamStride)
        {
            return std::unexpected(fmt::format(
                "material importer: param block {} bytes exceeds stride {}",
                blockReflected->Size, MaterialParamStride));
        }

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
                // A texture field with no 'id' is runtime-bound: the renderer
                // writes its bindless index per frame (the PostProcess input
                // handle). TextureAssetId stays 0, so the cook emits a handle
                // field with no resolved id and the loader patches no asset.
                if (fieldJson.contains("id"))
                {
                    if (!fieldJson["id"].is_number_unsigned())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: texture field '{}' 'id' must be an unsigned integer",
                            decl.Name));
                    }
                    decl.TextureAssetId = fieldJson["id"].get<u64>();
                }
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

        // --- 5. Build the zero-initialized block image and reflected lookup ---

        vector<u8> block(blockReflected->Size, 0);

        // name → reflected field, in the one block. Every declared field (handle
        // or param) validates against the single combined struct.
        std::map<string, const ReflectedStructField*> blockByName;
        for (const ReflectedStructField& f : blockReflected->Fields)
            blockByName[f.Name] = &f;

        // --- 6. Walk declared fields, routing each by type ---
        //
        // One CookedMaterialField per declared field — undeclared block members
        // (any pads) are validated/zeroed in the block image but not emitted, so a
        // Kind 0 entry always means an authored param.

        vector<CookedMaterialField> fields;
        fields.reserve(declaredFields.size());

        for (const DeclaredField& decl : declaredFields)
        {
            CookedMaterialField cookedField{};
            SetName(cookedField.Name, decl.Name);

            if (decl.Type == "texture" || decl.Type == "sampler")
            {
                // Handle fields are uint members of the one block.
                auto it = blockByName.find(decl.Name);
                if (it == blockByName.end())
                {
                    return std::unexpected(fmt::format(
                        "material importer: field '{}' does not match any field in MaterialParams",
                        decl.Name));
                }
                const ReflectedStructField& reflField = *it->second;

                if (reflField.IsFloat || reflField.ComponentCount != 1)
                {
                    return std::unexpected(fmt::format(
                        "material importer: {} field '{}' maps to a reflected field that is "
                        "not a scalar uint (IsFloat={}, ComponentCount={})",
                        decl.Type, decl.Name,
                        reflField.IsFloat ? "true" : "false",
                        reflField.ComponentCount));
                }

                cookedField.Offset = reflField.Offset;
                cookedField.Size = reflField.Size;

                if (decl.Type == "texture")
                {
                    // A runtime-bound texture field (no 'id') resolves no asset —
                    // it carries TextureId 0, and the renderer writes its bindless
                    // index per frame.
                    if (decl.TextureAssetId != 0)
                    {
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
                    }

                    cookedField.Kind = 1;
                    cookedField.TextureId = decl.TextureAssetId;
                }
                else
                {
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
            }
            else
            {
                // Param fields (uint / float / vecN) are members of the one block.
                auto it = blockByName.find(decl.Name);
                if (it == blockByName.end())
                {
                    return std::unexpected(fmt::format(
                        "material importer: field '{}' does not match any field in MaterialParams",
                        decl.Name));
                }
                const ReflectedStructField& reflField = *it->second;

                cookedField.Offset = reflField.Offset;
                cookedField.Size = reflField.Size;
                cookedField.Kind = 0;
                cookedField.TextureId = 0;

                if (decl.Type == "uint")
                {
                    if (reflField.IsFloat || reflField.ComponentCount != 1)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: uint field '{}' maps to a reflected field that is "
                            "not a scalar uint (IsFloat={}, ComponentCount={})",
                            decl.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount));
                    }

                    const usize writeEnd = static_cast<usize>(reflField.Offset) + sizeof(u32);
                    if (writeEnd > block.size())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: uint field '{}' at offset {} + 4 bytes "
                            "exceeds block size {}",
                            decl.Name, reflField.Offset, block.size()));
                    }

                    std::memcpy(block.data() + reflField.Offset, &decl.UintValue, sizeof(u32));
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

                    const usize writeEnd = static_cast<usize>(reflField.Offset) +
                        decl.FloatValues.size() * sizeof(f32);
                    if (writeEnd > block.size())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: {} field '{}' at offset {} + {} bytes "
                            "exceeds block size {}",
                            decl.Type, decl.Name,
                            reflField.Offset,
                            decl.FloatValues.size() * sizeof(f32),
                            block.size()));
                    }

                    // Write as little-endian f32 (host order == LE on macOS/x86).
                    std::memcpy(block.data() + reflField.Offset,
                        decl.FloatValues.data(),
                        decl.FloatValues.size() * sizeof(f32));
                }
            }

            fields.push_back(cookedField);
        }

        // --- 7. Assemble the blob ---

        CookedMaterialHeader header{};
        header.VertexShaderId = vertexShaderId;
        header.FragmentShaderId = fragmentShaderId;
        header.Version = CookedMaterialVersion;
        header.Domain = domain;
        header.FieldCount = static_cast<u32>(fields.size());
        header.BlockBytes = blockReflected->Size;

        return BuildBlob(header, fields, block);
    }
}
