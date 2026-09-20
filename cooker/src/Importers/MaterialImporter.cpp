#include "MaterialImporter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/Material.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/EnumName.h>

#include "GraphShaderSource.h"
#include "MaterialFieldEntry.h"
#include "SlangReflect.h"
#include "SlangSession.h"

namespace Veng::Cook
{
    namespace
    {
        // The largest parameter block one material may declare. Mirrors
        // Renderer::BindlessRegistry::MaxMaterialBlockBytes; restated here so the
        // cooker gains no renderer-header dependency.
        constexpr u32 MaxMaterialBlockBytes = 1280;

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h);
        // truncate rather than fail on an over-long identifier.
        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        // Splits an array member's "value" into one JSON value per element, so each reads as the
        // member's element type would on its own. A length other than the reflected element count
        // is a located error naming both figures: a five-entry table written with four values is
        // an authoring mistake, not a request to zero-fill the fifth.
        Result<vector<const json*>> SplitArrayValue(const json& value, std::string_view fieldName,
                                                    u32 elementCount)
        {
            if (!value.is_array())
            {
                return std::unexpected(
                    fmt::format("material importer: field '{}' reflects as an array of {} "
                                "elements, so its 'value' must be an array of {} element values",
                                fieldName, elementCount, elementCount));
            }
            if (value.size() != elementCount)
            {
                return std::unexpected(fmt::format(
                    "material importer: field '{}' 'value' has {} elements but the shader "
                    "reflects an array of {} elements",
                    fieldName, value.size(), elementCount));
            }

            vector<const json*> perElement;
            perElement.reserve(elementCount);
            for (const json& element : value)
            {
                perElement.push_back(&element);
            }
            return perElement;
        }

        // Assembles CookedMaterialHeader + CookedMaterialField[] + param block into one blob.
        vector<u8> BuildBlob(const CookedMaterialHeader& header,
                             const vector<CookedMaterialField>& fields, const vector<u8>& block)
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
        {
            return std::unexpected("material importer: missing or invalid 'source'");
        }

        const path vmatPath = context.PackDir / entry["source"].get<string>();

        const Result<json> vmatResult = ReadJsonFile(vmatPath, "material importer");
        if (!vmatResult)
        {
            return std::unexpected(vmatResult.error());
        }
        const json& vmat = *vmatResult;

        // --- 1b. Parse the optional domain (default Surface) ---

        // Absent → Surface; unknown value is a located cook error.
        MaterialDomain domainValue = MaterialDomain::Surface;
        if (vmat.contains("domain"))
        {
            if (!vmat["domain"].is_string())
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': 'domain' must be a string "
                                "(\"Surface\", \"PostProcess\", \"Sky\", \"Translucent\", or "
                                "\"GuiFill\")",
                                vmatPath.string()));
            }
            const string domainStr = vmat["domain"].get<string>();
            const optional<MaterialDomain> parsed = ParseEnum<MaterialDomain>(domainStr);
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': unknown domain '{}' "
                                "(expected \"Surface\", \"PostProcess\", \"Sky\", "
                                "\"Translucent\", or \"GuiFill\")",
                                vmatPath.string(), domainStr));
            }
            domainValue = *parsed;
        }
        const u32 domain = static_cast<u32>(domainValue);

        // --- 1c. Parse the optional cull mode (default Back) ---

        // Absent → Back, the engine's geometry-pipeline convention; unknown value is a
        // located cook error. FrontAndBack rasterizes nothing, so it is not authorable.
        Renderer::CullMode cullValue = Renderer::CullMode::Back;
        if (vmat.contains("cull"))
        {
            if (!vmat["cull"].is_string())
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': 'cull' must be a string "
                                "(\"Back\", \"Front\", or \"None\")",
                                vmatPath.string()));
            }
            const string cullStr = vmat["cull"].get<string>();
            const optional<Renderer::CullMode> parsed = ParseEnum<Renderer::CullMode>(cullStr);
            if (!parsed || *parsed == Renderer::CullMode::FrontAndBack)
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': unknown cull mode '{}' "
                                "(expected \"Back\", \"Front\", or \"None\")",
                                vmatPath.string(), cullStr));
            }
            cullValue = *parsed;
        }
        const u32 cull = static_cast<u32>(cullValue);

        // --- 1d. Parse the optional translucent sort priority (default 0) ---

        // Draw-order priority for Translucent materials (back-to-front within ascending
        // priority groups). Accepted on any domain but read only by the translucent pass.
        i32 sortPriority = 0;
        if (vmat.contains("sortPriority"))
        {
            if (!vmat["sortPriority"].is_number_integer())
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': 'sortPriority' must be an integer",
                                vmatPath.string()));
            }
            sortPriority = vmat["sortPriority"].get<i32>();
        }

        // --- 1e. Parse the optional bloom-mask flag (default false) ---

        // Set by a Translucent material whose fragment returns TranslucentBloomOutput — a second
        // color output naming the glow the surface wants apart from how bright it is. Accepted on
        // any domain but read only by the translucent pass, which enables the mask attachment's
        // writes for a declaring material's pipeline alone.
        bool bloomMask = false;
        if (vmat.contains("bloomMask"))
        {
            if (!vmat["bloomMask"].is_boolean())
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': 'bloomMask' must be a boolean", vmatPath.string()));
            }
            bloomMask = vmat["bloomMask"].get<bool>();
        }

        // --- 1f. Parse the optional resolution (default full) ---

        // "half" opts a Translucent material into the reduced-resolution translucent layer — a
        // half-resolution offscreen composited under the full-resolution translucents. The layer
        // carries no bloom-mask attachment, so the two keys are exclusive; and a non-Translucent
        // domain has no layer to opt into, so the key is a located cook error there rather than
        // silently inert.
        bool halfResolution = false;
        if (vmat.contains("resolution"))
        {
            if (!vmat["resolution"].is_string())
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': 'resolution' must be a string "
                                "(\"full\" or \"half\")",
                                vmatPath.string()));
            }
            const string resolutionStr = vmat["resolution"].get<string>();
            if (resolutionStr == "half")
            {
                halfResolution = true;
            }
            else if (resolutionStr != "full")
            {
                return std::unexpected(
                    fmt::format("material importer: '{}': unknown resolution '{}' "
                                "(expected \"full\" or \"half\")",
                                vmatPath.string(), resolutionStr));
            }
            if (halfResolution && domainValue != MaterialDomain::Translucent)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': 'resolution: half' requires the Translucent domain",
                    vmatPath.string()));
            }
            if (halfResolution && bloomMask)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': 'resolution: half' is exclusive with 'bloomMask' — "
                    "the reduced-resolution layer carries no mask attachment",
                    vmatPath.string()));
            }
        }

        // --- 2. Validate and resolve shader references ---

        if (!vmat.contains("shaders") || !vmat["shaders"].is_object())
        {
            return std::unexpected("material importer: missing or invalid 'shaders' object");
        }

        const json& shaders = vmat["shaders"];

        if (!shaders.contains("vertex") || !shaders["vertex"].is_string())
        {
            return std::unexpected("material importer: 'shaders.vertex' must be a hex id string");
        }

        if (!shaders.contains("fragment") || !shaders["fragment"].is_string())
        {
            return std::unexpected("material importer: 'shaders.fragment' must be a hex id string");
        }

        const optional<AssetId> vertexParsed = ParseAssetId(shaders["vertex"].get<string>());
        if (!vertexParsed)
        {
            return std::unexpected(
                fmt::format("material importer: 'shaders.vertex' is a malformed hex id '{}'",
                            shaders["vertex"].get<string>()));
        }

        const optional<AssetId> fragmentParsed = ParseAssetId(shaders["fragment"].get<string>());
        if (!fragmentParsed)
        {
            return std::unexpected(
                fmt::format("material importer: 'shaders.fragment' is a malformed hex id '{}'",
                            shaders["fragment"].get<string>()));
        }

        const u64 vertexShaderId = vertexParsed->Value;
        const u64 fragmentShaderId = fragmentParsed->Value;

        if (!context.Resolve)
        {
            return std::unexpected("material importer: no resolver available");
        }

        const optional<ResolvedSource> vertexResolved =
            context.Resolve(AssetId{.Value = vertexShaderId});
        if (!vertexResolved)
        {
            return std::unexpected(fmt::format(
                "material importer: vertex shader {} not found in pack or reference packs",
                vertexShaderId));
        }
        if (vertexResolved->Type != AssetTypes::Shader)
        {
            return std::unexpected(fmt::format(
                "material importer: asset {} referenced as vertex shader but has type {}",
                vertexShaderId, FormatHexId(vertexResolved->Type.Value)));
        }

        const optional<ResolvedSource> fragmentResolved =
            context.Resolve(AssetId{.Value = fragmentShaderId});
        if (!fragmentResolved)
        {
            return std::unexpected(fmt::format(
                "material importer: fragment shader {} not found in pack or reference packs",
                fragmentShaderId));
        }
        if (fragmentResolved->Type != AssetTypes::Shader)
        {
            return std::unexpected(fmt::format(
                "material importer: asset {} referenced as fragment shader but has type {}",
                fragmentShaderId, FormatHexId(fragmentResolved->Type.Value)));
        }

        // The fragment shader supplies the param-block layout, so it must resolve
        // to a source path the cook can reflect.
        if (fragmentResolved->AbsolutePath.empty())
        {
            return std::unexpected(
                fmt::format("material importer: fragment shader {} has no resolvable source path",
                            fragmentShaderId));
        }

        // --- 3. Reflect the param block from the fragment shader's Slang source ---

        // The fragment shader's AbsolutePath points at its *.shader.json, not a .slang.
        // Read that JSON to locate the actual .slang source relative to its directory.
        const path shaderJsonPath = fragmentResolved->AbsolutePath;

        const Result<json> shaderJsonResult = ReadJsonFile(shaderJsonPath, "material importer");
        if (!shaderJsonResult)
        {
            return std::unexpected(shaderJsonResult.error());
        }
        const json& shaderJson = *shaderJsonResult;

        if (!shaderJson.contains("source") || !shaderJson["source"].is_string())
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': missing or invalid 'source'", shaderJsonPath.string()));
        }

        if (!shaderJson.contains("entry") || !shaderJson["entry"].is_string())
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': missing or invalid 'entry'", shaderJsonPath.string()));
        }
        const string fragEntry = shaderJson["entry"].get<string>();

        // The fragment shader's source is either a .slang file or a node graph. A graph
        // source has no .slang on disk, so reflect the generated text — the same walk the
        // ShaderImporter compiled, so the reflected MaterialParams matches the cooked
        // shader's exactly.
        const Result<GraphShaderSource> graphSource =
            ResolveGraphShaderSourceHook(shaderJson, shaderJsonPath.parent_path());
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
            fragSource = SlangModuleSource{.Path = shaderJsonPath.parent_path() /
                                                   shaderJson["source"].get<string>()};
        }

        // --- 3b. Validate the fragment outputs against the domain's contract ---

        // Surface: float4 SV_Target0 (albedo) + SV_Target1 (normal) + SV_Target2 (ORM) +
        // float2 SV_Target3 (screen-space motion vector) + float3 SV_Target4 (HDR emissive).
        // PostProcess: single float4 SV_Target0. Sky: single float4 SV_Target0 (background
        // radiance, not a g-buffer MRT). Translucent: single float4 SV_Target0 (final HDR color +
        // alpha, forward-blended into the scene, not a g-buffer MRT), plus a float SV_Target1 when
        // the material declares "bloomMask". GuiFill: single float4
        // SV_Target0 (premultiplied UI fill). Mismatch is a located cook error. An SV_Depth
        // member is not a color target and never reaches this check — the reflection drops it,
        // so any domain may write depth alongside its targets.
        const Result<vector<ReflectedFragmentOutput>> outputs =
            ReflectFragmentOutputs(fragSource, fragEntry, context.ShaderIncludeDir);
        if (!outputs)
        {
            return std::unexpected(outputs.error());
        }

        if (domainValue == MaterialDomain::Surface)
        {
            const bool ok = outputs->size() == 5 && (*outputs)[0].TargetIndex == 0 &&
                            (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4 &&
                            (*outputs)[1].TargetIndex == 1 && (*outputs)[1].IsFloat &&
                            (*outputs)[1].ComponentCount == 4 && (*outputs)[2].TargetIndex == 2 &&
                            (*outputs)[2].IsFloat && (*outputs)[2].ComponentCount == 4 &&
                            (*outputs)[3].TargetIndex == 3 && (*outputs)[3].IsFloat &&
                            (*outputs)[3].ComponentCount == 2 && (*outputs)[4].TargetIndex == 4 &&
                            (*outputs)[4].IsFloat && (*outputs)[4].ComponentCount == 3;
            if (!ok)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': surface material must write the g-buffer "
                    "(float4 SV_Target0 + float4 SV_Target1 + float4 SV_Target2 + "
                    "float2 SV_Target3 + float3 SV_Target4); its fragment shader does not",
                    vmatPath.string()));
            }
        }
        else if (domainValue == MaterialDomain::Sky)
        {
            // A Sky material outputs radiance into the single scene-color target, not the
            // g-buffer MRT — a single float4 SV_Target0. A sky fragment that writes the
            // g-buffer set (or any further target) violates the sky-slot contract.
            const bool ok = outputs->size() == 1 && (*outputs)[0].TargetIndex == 0 &&
                            (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4;
            if (!ok)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': sky material must write a single float4 SV_Target0 "
                    "(background radiance) and no further targets — not the g-buffer MRT; its "
                    "fragment shader does not",
                    vmatPath.string()));
            }
        }
        else if (domainValue == MaterialDomain::Translucent)
        {
            // A Translucent material outputs final HDR color + alpha into the single scene-color
            // target, forward-blended, not the g-buffer MRT — a single float4 SV_Target0. A
            // fragment that writes the g-buffer set (or any further target) violates the contract.
            // Declaring "bloomMask" adds exactly one more: a scalar float SV_Target1 carrying the
            // glow strength the surface asks for apart from its luminance. The two must agree —
            // the pass enables the mask attachment's writes from the flag, so a flag without the
            // output writes undefined values into the mask and an output without the flag is
            // silently discarded.
            const bool colorOk = !outputs->empty() && (*outputs)[0].TargetIndex == 0 &&
                                 (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4;
            const bool maskOk =
                !bloomMask ? outputs->size() == 1
                           : outputs->size() == 2 && (*outputs)[1].TargetIndex == 1 &&
                                 (*outputs)[1].IsFloat && (*outputs)[1].ComponentCount == 1;
            if (!colorOk || !maskOk)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': translucent material must write a single float4 "
                    "SV_Target0 (HDR color + alpha){} — not the g-buffer MRT; its fragment shader "
                    "does not",
                    vmatPath.string(),
                    bloomMask ? " plus a float SV_Target1 (the bloom mask it declares) and no "
                                "further targets"
                              : " and no further targets"));
            }
        }
        else if (domainValue == MaterialDomain::GuiFill)
        {
            // A GuiFill material writes the premultiplied linear fill of one UI quad into the
            // GUI pass's single color target — a single float4 SV_Target0.
            const bool ok = outputs->size() == 1 && (*outputs)[0].TargetIndex == 0 &&
                            (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4;
            if (!ok)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': gui fill material must write a single float4 "
                    "SV_Target0 (premultiplied UI fill) and no further targets; its fragment "
                    "shader does not",
                    vmatPath.string()));
            }
        }
        else // PostProcess
        {
            // A PostProcess material writes a single float4 SV_Target0 (the composited color).
            // Declaring "bloomMask" adds exactly one more — a scalar float SV_Target1 carrying the
            // glow amplitude apart from the color's luminance, the same second-output convention the
            // Translucent domain uses. A fullscreen composite that seeds the bloom pyramid from a
            // mask (a document composited into scene HDR pre-bloom) is the case this serves; a plain
            // effect leaves the flag off and writes SV_Target0 alone. The flag and the output must
            // agree: the consuming pass enables the mask attachment's writes from the flag, so a flag
            // without the output writes undefined values into the mask and an output without the flag
            // is silently discarded.
            const bool colorOk = !outputs->empty() && (*outputs)[0].TargetIndex == 0 &&
                                 (*outputs)[0].IsFloat && (*outputs)[0].ComponentCount == 4;
            const bool maskOk =
                !bloomMask ? outputs->size() == 1
                           : outputs->size() == 2 && (*outputs)[1].TargetIndex == 1 &&
                                 (*outputs)[1].IsFloat && (*outputs)[1].ComponentCount == 1;
            if (!colorOk || !maskOk)
            {
                return std::unexpected(fmt::format(
                    "material importer: '{}': postprocess material must write a single "
                    "float4 SV_Target0{}; its fragment shader does not",
                    vmatPath.string(),
                    bloomMask ? " plus a float SV_Target1 (the bloom mask it declares) and no "
                                "further targets"
                              : " and no further targets"));
            }
        }

        // MaterialParams is optional — a fieldless material declares no struct,
        // which reflects as an empty (Size 0) layout.
        const Result<ReflectedStruct> blockReflected = ReflectStructLayout(
            fragSource, "MaterialParams", context.ShaderIncludeDir, /*optional=*/true);
        if (!blockReflected)
        {
            return std::unexpected(blockReflected.error());
        }

        if (blockReflected->Size > MaxMaterialBlockBytes)
        {
            return std::unexpected(fmt::format(
                "material importer: param block {} bytes exceeds the per-material bound of {}",
                blockReflected->Size, MaxMaterialBlockBytes));
        }

        // --- 4. Build the zero-initialized block image and the reflected lookup ---

        vector<u8> block(blockReflected->Size, 0);

        // name → reflected field, in the one block. Every declared field (handle or param)
        // validates against the single combined struct, and an entry that states no type takes its
        // type from here.
        std::map<string, const ReflectedStructField*> blockByName;
        for (const ReflectedStructField& f : blockReflected->Fields)
        {
            blockByName[f.Name] = &f;
        }

        // --- 5. Parse vmat["fields"] into an ordered declared-field list ---

        if (!vmat.contains("fields") || !vmat["fields"].is_array())
        {
            return std::unexpected("material importer: missing or invalid 'fields' array");
        }

        struct DeclaredField
        {
            string Name;
            string Type; // canonical, resolved against the reflected member
            const ReflectedStructField* Reflected = nullptr;
            // texture / sampler
            u64 TextureAssetId = 0;    // texture: the asset id
            string SamplerTextureName; // sampler: the name of the referenced texture field
            // float / vecN / uint — an empty value list leaves the member at the block's zero.
            // An array member's values are flattened in element order.
            vector<f32> FloatValues;
            vector<u32> UintValues;
        };

        vector<DeclaredField> declaredFields;
        // name → index in declaredFields, for duplicate detection and sampler lookup
        std::map<string, usize> declaredByName;

        for (const json& fieldJson : vmat["fields"])
        {
            const Result<MaterialFieldEntry> resolved =
                ResolveMaterialFieldEntry(fieldJson, blockByName, "material importer");
            if (!resolved)
            {
                return std::unexpected(resolved.error());
            }
            if (resolved->Reflected == nullptr)
            {
                return std::unexpected(fmt::format(
                    "material importer: field '{}' does not match any field in MaterialParams",
                    resolved->Name));
            }

            DeclaredField decl;
            decl.Name = resolved->Name;
            decl.Type = resolved->Type;
            decl.Reflected = resolved->Reflected;

            if (declaredByName.count(decl.Name))
            {
                return std::unexpected(
                    fmt::format("material importer: duplicate declared field '{}'", decl.Name));
            }

            // A bare-string entry states the member's name and nothing else; the object form's
            // remaining keys are read here.
            const json* body = fieldJson.is_object() ? &fieldJson : nullptr;
            const json* value =
                (body != nullptr && body->contains("value")) ? &(*body)["value"] : nullptr;

            if (decl.Type == "texture")
            {
                // A texture field with no 'id' is runtime-bound: the renderer
                // writes its bindless index per frame (the PostProcess input
                // handle). TextureAssetId stays 0, so the cook emits a handle
                // field with no resolved id and the loader patches no asset.
                if (body != nullptr && body->contains("id"))
                {
                    if (!(*body)["id"].is_string())
                    {
                        return std::unexpected(fmt::format("material importer: texture field '{}' "
                                                           "'id' must be a hex id string",
                                                           decl.Name));
                    }
                    const optional<AssetId> parsed = ParseAssetId((*body)["id"].get<string>());
                    if (!parsed)
                    {
                        return std::unexpected(fmt::format("material importer: texture field '{}' "
                                                           "'id' is a malformed hex id '{}'",
                                                           decl.Name, (*body)["id"].get<string>()));
                    }
                    decl.TextureAssetId = parsed->Value;
                }
            }
            else if (decl.Type == "sampler")
            {
                if (body == nullptr || !body->contains("texture") ||
                    !(*body)["texture"].is_string())
                {
                    return std::unexpected(fmt::format(
                        "material importer: sampler field '{}' must have a 'texture' name",
                        decl.Name));
                }
                decl.SamplerTextureName = (*body)["texture"].get<string>();
            }
            else if (decl.Type == "uint")
            {
                if (value != nullptr)
                {
                    const u32 elements = decl.Reflected->ElementCount;
                    if (elements > 1)
                    {
                        const Result<vector<const json*>> perElement =
                            SplitArrayValue(*value, decl.Name, elements);
                        if (!perElement)
                        {
                            return std::unexpected(perElement.error());
                        }
                        for (const json* element : *perElement)
                        {
                            if (!element->is_number_unsigned())
                            {
                                return std::unexpected(fmt::format(
                                    "material importer: uint field '{}' 'value' array contains a "
                                    "non-unsigned-integer element",
                                    decl.Name));
                            }
                            decl.UintValues.push_back(element->get<u32>());
                        }
                    }
                    else if (!value->is_number_unsigned())
                    {
                        return std::unexpected(
                            fmt::format("material importer: uint field '{}' 'value' must be an "
                                        "unsigned integer",
                                        decl.Name));
                    }
                    else
                    {
                        decl.UintValues.push_back(value->get<u32>());
                    }
                }
            }
            else if (decl.Type == "storagebuffer" || decl.Type == "volume")
            {
                // Always runtime-bound: the consumer registers the buffer (or the 3D texture,
                // which has no cooked asset) and writes its bindless index per frame. Neither
                // carries a cooked default, so nothing more is parsed here.
            }
            else
            {
                // float / vec2 / vec3 / vec4, and arrays of those. 'value' is optional: an absent
                // one leaves the member at the block image's zero, which is the default the array
                // would have spelled out. Arity and value form both come from reflection, the
                // single source of truth for the member's type.
                const u32 components = decl.Reflected->ComponentCount;
                const u32 elements = decl.Reflected->ElementCount;
                if (value != nullptr)
                {
                    // An array field's value is one entry per element, so the per-element values
                    // below read exactly as a non-array member's would.
                    vector<const json*> perElement;
                    if (elements > 1)
                    {
                        const Result<vector<const json*>> split =
                            SplitArrayValue(*value, decl.Name, elements);
                        if (!split)
                        {
                            return std::unexpected(split.error());
                        }
                        perElement = *split;
                    }
                    else
                    {
                        perElement.push_back(value);
                    }

                    for (const json* element : perElement)
                    {
                        if (components == 1)
                        {
                            if (!element->is_number())
                            {
                                return std::unexpected(fmt::format(
                                    "material importer: float field '{}' 'value' must be a number",
                                    decl.Name));
                            }
                            decl.FloatValues.push_back(element->get<f32>());
                            continue;
                        }

                        if (!element->is_array())
                        {
                            return std::unexpected(
                                fmt::format("material importer: {} field '{}' 'value' must be an "
                                            "array of {} numbers",
                                            decl.Type, decl.Name, components));
                        }
                        if (element->size() != components)
                        {
                            return std::unexpected(
                                fmt::format("material importer: field '{}' 'value' has {} elements "
                                            "but the shader reflects {} components",
                                            decl.Name, element->size(), components));
                        }
                        for (const json& scalar : *element)
                        {
                            if (!scalar.is_number())
                            {
                                return std::unexpected(
                                    fmt::format("material importer: {} field '{}' 'value' array "
                                                "contains a non-number element",
                                                decl.Type, decl.Name));
                            }
                            decl.FloatValues.push_back(scalar.get<f32>());
                        }
                    }
                }
            }

            const usize idx = declaredFields.size();
            declaredFields.push_back(std::move(decl));
            declaredByName[declaredFields[idx].Name] = idx;
        }

        // Every member of MaterialParams is declared by the material. This is the direction the walk
        // below cannot cover: it validates each *declared* field against the struct, so a member the
        // shader declares and the material omits was never reached. It stayed zero in the block
        // image, the cook succeeded, and the fragment read zero for the life of the asset — and
        // where zero is not inert for that parameter (a scale, a radius, a count, a coverage) the
        // surface drew wrong with nothing naming the missing line. The opposite direction has always
        // been an error, so this closes the pair.
        //
        // **The rule is total: there is no exemption for padding.** The block is packed in scalar/
        // tight layout (SlangReflect), the same layout the shader's Load<T> reads, so a struct needs
        // no alignment pad and field order carries no correctness weight. Exempting an undeclared
        // spelling would put a hole in the contract exactly where a real parameter could hide.
        {
            string undeclared;
            for (const ReflectedStructField& reflField : blockReflected->Fields)
            {
                if (declaredByName.find(reflField.Name) == declaredByName.end())
                {
                    if (!undeclared.empty())
                    {
                        undeclared += ", ";
                    }
                    undeclared += reflField.Name;
                }
            }
            if (!undeclared.empty())
            {
                return std::unexpected(fmt::format(
                    "material importer: MaterialParams members with no entry in the material's "
                    "\"fields\" (the member's name alone is an entry): {}",
                    undeclared));
            }
        }

        // --- 6. Walk declared fields, routing each by type ---
        //
        // One CookedMaterialField per declared field — undeclared block members
        // (any pads) are validated/zeroed in the block image but not emitted, so a
        // Kind 0 entry always means an authored param.

        vector<CookedMaterialField> fields;
        fields.reserve(declaredFields.size());

        for (const DeclaredField& decl : declaredFields)
        {
            // A flattened nested member's dotted name can outgrow the cooked name field, and
            // SetName truncates — which would silently merge two members into one entry.
            if (decl.Name.size() >= ShaderNameCapacity)
            {
                return std::unexpected(fmt::format(
                    "material importer: field name '{}' is {} bytes, past the {}-byte cooked "
                    "field-name capacity",
                    decl.Name, decl.Name.size(), ShaderNameCapacity - 1));
            }

            CookedMaterialField cookedField{};
            SetName(cookedField.Name, decl.Name);
            cookedField.ElementCount = decl.Reflected->ElementCount;
            cookedField.ElementStride = decl.Reflected->ElementStride;

            if (decl.Type == "texture" || decl.Type == "volume" || decl.Type == "sampler" ||
                decl.Type == "storagebuffer")
            {
                // Handle fields are uint members of the one block.
                const ReflectedStructField& reflField = *decl.Reflected;

                cookedField.Offset = reflField.Offset;
                cookedField.Size = reflField.Size;

                if (decl.Type == "storagebuffer")
                {
                    // A runtime-bound storage-buffer handle (Kind 3): no cooked asset, its
                    // bindless index written per frame via SetStorageBufferHandle.
                    cookedField.Kind = 3;
                    cookedField.TextureId = 0;
                }
                else if (decl.Type == "volume")
                {
                    // A runtime-bound 3D sampled-image (volume) handle (Kind 4): no cooked 3D
                    // texture asset, its bindless index written per frame via SetVolumeHandle.
                    cookedField.Kind = 4;
                    cookedField.TextureId = 0;
                }
                else if (decl.Type == "texture")
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
                            return std::unexpected(
                                fmt::format("material importer: texture {} for field '{}' not "
                                            "found in pack or reference packs",
                                            decl.TextureAssetId, decl.Name));
                        }
                        if (texResolved->Type != AssetTypes::Texture)
                        {
                            return std::unexpected(
                                fmt::format("material importer: asset {} referenced as texture for "
                                            "field '{}' but has type {}",
                                            decl.TextureAssetId, decl.Name,
                                            FormatHexId(texResolved->Type.Value)));
                        }
                    }

                    cookedField.Kind = 1;
                    cookedField.TextureId = decl.TextureAssetId;
                }
                else
                {
                    // Look up the referenced texture-or-volume field by name among declared
                    // fields — a sampler pairs with a 2D texture or a 3D volume alike.
                    auto texDeclIt = declaredByName.find(decl.SamplerTextureName);
                    const bool refIsSampled =
                        texDeclIt != declaredByName.end() &&
                        (declaredFields[texDeclIt->second].Type == "texture" ||
                         declaredFields[texDeclIt->second].Type == "volume");
                    if (!refIsSampled)
                    {
                        return std::unexpected(fmt::format(
                            "material importer: sampler field '{}' references '{}' which is not a "
                            "declared texture or volume field",
                            decl.Name, decl.SamplerTextureName));
                    }

                    cookedField.Kind = 2;
                    cookedField.TextureId = declaredFields[texDeclIt->second].TextureAssetId;
                }
            }
            else
            {
                // Param fields (uint / float / vecN) are members of the one block.
                const ReflectedStructField& reflField = *decl.Reflected;

                cookedField.Offset = reflField.Offset;
                cookedField.Size = reflField.Size;
                cookedField.Kind = 0;
                cookedField.TextureId = 0;

                if (decl.Type == "uint")
                {
                    const usize writeEnd =
                        static_cast<usize>(reflField.Offset) + decl.UintValues.size() * sizeof(u32);
                    if (writeEnd > block.size())
                    {
                        return std::unexpected(
                            fmt::format("material importer: uint field '{}' at offset {} + {} "
                                        "bytes exceeds block size {}",
                                        decl.Name, reflField.Offset,
                                        decl.UintValues.size() * sizeof(u32), block.size()));
                    }

                    if (!decl.UintValues.empty())
                    {
                        std::memcpy(block.data() + reflField.Offset, decl.UintValues.data(),
                                    decl.UintValues.size() * sizeof(u32));
                    }
                }
                else
                {
                    // float / vec2 / vec3 / vec4, and arrays of those: the tight stride makes an
                    // array's elements contiguous, so the flattened values are one copy.
                    // FloatValues is empty when the entry states no 'value', which leaves the
                    // member at the block image's zero.
                    const usize writeEnd = static_cast<usize>(reflField.Offset) +
                                           decl.FloatValues.size() * sizeof(f32);
                    if (writeEnd > block.size())
                    {
                        return std::unexpected(
                            fmt::format("material importer: {} field '{}' at offset {} + {} bytes "
                                        "exceeds block size {}",
                                        decl.Type, decl.Name, reflField.Offset,
                                        decl.FloatValues.size() * sizeof(f32), block.size()));
                    }

                    // Write as little-endian f32 (host order == LE on macOS/x86).
                    if (!decl.FloatValues.empty())
                    {
                        std::memcpy(block.data() + reflField.Offset, decl.FloatValues.data(),
                                    decl.FloatValues.size() * sizeof(f32));
                    }
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
        header.CullMode = cull;
        header.SortPriority = sortPriority;
        header.BloomMask = bloomMask ? 1u : 0u;
        header.HalfResolution = halfResolution ? 1u : 0u;
        header.FieldCount = static_cast<u32>(fields.size());
        header.BlockBytes = blockReflected->Size;

        return BuildBlob(header, fields, block);
    }
}
