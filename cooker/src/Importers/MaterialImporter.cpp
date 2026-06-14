#include "MaterialImporter.h"

#include <algorithm>
#include <cstring>

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
        // --- 1. Validate and resolve shader references ---

        if (!entry.contains("shaders") || !entry["shaders"].is_object())
            return std::unexpected("material importer: missing or invalid 'shaders' object");

        const json& shaders = entry["shaders"];

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

        // Fragment shader must have a source path to reflect MaterialData from.
        // Inline spirv_b64-only shaders have no AbsolutePath to compile.
        if (fragmentResolved->AbsolutePath.empty())
        {
            return std::unexpected(fmt::format(
                "material importer: fragment shader {} has no resolvable source path — "
                "inline-only materials are out of scope this phase",
                fragmentShaderId));
        }

        // --- 2. Reflect MaterialData from the fragment shader's source ---

        const Result<ReflectedStruct> reflected =
            ReflectStructLayout(fragmentResolved->AbsolutePath, "MaterialData");
        if (!reflected)
            return std::unexpected(reflected.error());

        // --- 3. Build the zero-initialised param block ---

        const u32 paramBytes = reflected->Size;
        vector<u8> paramBlock(paramBytes, 0);

        // --- 4. Classify fields and build the CookedMaterialField table ---

        // Collect the textures map (field name → texture AssetId).
        // Default to empty object if absent.
        const json& texturesJson = entry.contains("textures") && entry["textures"].is_object()
            ? entry["textures"]
            : json::object();

        // Collect the params map (field name → value).
        const json& paramsJson = entry.contains("params") && entry["params"].is_object()
            ? entry["params"]
            : json::object();

        // Track which textures/params keys were consumed.
        vector<string> matchedTextureKeys;
        vector<string> matchedParamKeys;

        // Build a set of texture key names for quick lookup.
        // Also build the sampler pairing: <key>Sampler → key.
        // We discover sampler-paired fields by checking if the field name ends with
        // "Sampler" and the prefix matches a texture key.
        vector<CookedMaterialField> fields;
        fields.reserve(reflected->Fields.size());

        for (const ReflectedStructField& reflField : reflected->Fields)
        {
            CookedMaterialField cookedField{};
            SetName(cookedField.Name, reflField.Name);
            cookedField.Offset = reflField.Offset;
            cookedField.Size = reflField.Size;

            // Check if this field is a texture handle (Kind 1).
            bool classified = false;
            for (auto it = texturesJson.begin(); it != texturesJson.end(); ++it)
            {
                if (it.key() == reflField.Name)
                {
                    // Must be a uint handle slot.
                    if (reflField.IsFloat || reflField.ComponentCount != 1)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: textures key '{}' maps to field '{}' "
                            "but the field is not a scalar uint (IsFloat={}, ComponentCount={})",
                            it.key(), reflField.Name,
                            reflField.IsFloat ? "true" : "false",
                            reflField.ComponentCount));
                    }

                    if (!it.value().is_number_unsigned())
                    {
                        return std::unexpected(fmt::format(
                            "material importer: textures['{}'] must be an unsigned integer AssetId",
                            it.key()));
                    }

                    const u64 textureId = it.value().get<u64>();

                    // Validate the texture asset exists and is AssetType::Texture.
                    const optional<ResolvedSource> texResolved = context.Resolve(AssetId{.Value = textureId});
                    if (!texResolved)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: texture {} for field '{}' not found in pack or reference packs",
                            textureId, reflField.Name));
                    }
                    if (texResolved->Type != AssetType::Texture)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: asset {} referenced as texture for field '{}' but has type {}",
                            textureId, reflField.Name, static_cast<u32>(texResolved->Type)));
                    }

                    cookedField.Kind = 1;
                    cookedField.TextureId = textureId;
                    matchedTextureKeys.push_back(it.key());
                    classified = true;
                    break;
                }
            }

            if (!classified)
            {
                // Check if this field is a sampler handle (Kind 2): named "<textureKey>Sampler".
                for (auto it = texturesJson.begin(); it != texturesJson.end(); ++it)
                {
                    const string samplerName = it.key() + "Sampler";
                    if (samplerName == reflField.Name)
                    {
                        // Must be a scalar uint.
                        if (reflField.IsFloat || reflField.ComponentCount != 1)
                        {
                            return std::unexpected(fmt::format(
                                "material importer: sampler field '{}' (paired with texture '{}') "
                                "is not a scalar uint (IsFloat={}, ComponentCount={})",
                                reflField.Name, it.key(),
                                reflField.IsFloat ? "true" : "false",
                                reflField.ComponentCount));
                        }

                        if (!it.value().is_number_unsigned())
                        {
                            return std::unexpected(fmt::format(
                                "material importer: textures['{}'] must be an unsigned integer AssetId",
                                it.key()));
                        }

                        const u64 textureId = it.value().get<u64>();
                        cookedField.Kind = 2;
                        cookedField.TextureId = textureId;
                        // Sampler fields are implicitly matched by texture key; no separate
                        // tracking needed since we validate texture keys are consumed below.
                        classified = true;
                        break;
                    }
                }
            }

            if (!classified)
            {
                // Kind 0: param or ignored pad.
                cookedField.Kind = 0;
                cookedField.TextureId = 0;

                // If the field is named in params, pack its value.
                for (auto it = paramsJson.begin(); it != paramsJson.end(); ++it)
                {
                    if (it.key() == reflField.Name)
                    {
                        // Field must be float-typed to accept a param value.
                        if (!reflField.IsFloat)
                        {
                            return std::unexpected(fmt::format(
                                "material importer: params key '{}' maps to field '{}' "
                                "but the field is not a float type",
                                it.key(), reflField.Name));
                        }

                        const json& val = it.value();

                        // Gather the float values from JSON.
                        vector<f32> floats;
                        if (val.is_number())
                        {
                            floats.push_back(val.get<f32>());
                        }
                        else if (val.is_array())
                        {
                            for (const json& elem : val)
                            {
                                if (!elem.is_number())
                                {
                                    return std::unexpected(fmt::format(
                                        "material importer: params['{}'] array contains a non-number element",
                                        it.key()));
                                }
                                floats.push_back(elem.get<f32>());
                            }
                        }
                        else
                        {
                            return std::unexpected(fmt::format(
                                "material importer: params['{}'] must be a number or array of numbers",
                                it.key()));
                        }

                        // Component count must match.
                        if (static_cast<u32>(floats.size()) != reflField.ComponentCount)
                        {
                            return std::unexpected(fmt::format(
                                "material importer: params['{}'] has {} value(s) but field '{}' "
                                "has ComponentCount {}",
                                it.key(), floats.size(), reflField.Name, reflField.ComponentCount));
                        }

                        // Bounds check before writing.
                        const usize writeEnd = static_cast<usize>(reflField.Offset) + floats.size() * sizeof(f32);
                        if (writeEnd > paramBlock.size())
                        {
                            return std::unexpected(fmt::format(
                                "material importer: params['{}'] field '{}' at offset {} + {} bytes "
                                "exceeds param block size {}",
                                it.key(), reflField.Name,
                                reflField.Offset, floats.size() * sizeof(f32),
                                paramBlock.size()));
                        }

                        // Write as little-endian f32 (host order == LE on macOS/x86).
                        std::memcpy(paramBlock.data() + reflField.Offset,
                            floats.data(), floats.size() * sizeof(f32));

                        matchedParamKeys.push_back(it.key());
                        break;
                    }
                }
            }

            fields.push_back(cookedField);
        }

        // --- 5. Validate that every textures/params key matched a field ---

        for (auto it = texturesJson.begin(); it != texturesJson.end(); ++it)
        {
            const bool consumed = std::find(matchedTextureKeys.begin(), matchedTextureKeys.end(), it.key())
                != matchedTextureKeys.end();
            if (!consumed)
            {
                return std::unexpected(fmt::format(
                    "material importer: textures key '{}' does not match any field in MaterialData",
                    it.key()));
            }
        }

        for (auto it = paramsJson.begin(); it != paramsJson.end(); ++it)
        {
            const bool consumed = std::find(matchedParamKeys.begin(), matchedParamKeys.end(), it.key())
                != matchedParamKeys.end();
            if (!consumed)
            {
                return std::unexpected(fmt::format(
                    "material importer: params key '{}' does not match any field in MaterialData",
                    it.key()));
            }
        }

        // --- 6. Assemble the blob ---

        CookedMaterialHeader header{};
        header.VertexShaderId = vertexShaderId;
        header.FragmentShaderId = fragmentShaderId;
        header.FieldCount = static_cast<u32>(fields.size());
        header.ParamBytes = paramBytes;

        return BuildBlob(header, fields, paramBlock);
    }
}
