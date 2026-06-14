#include "ShaderImporter.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include <Veng/Asset/CookedBlobs.h>

#include "VertexLayoutSource.h"

namespace Veng::Cook
{
    namespace
    {
        using Slang::ComPtr;

        // The underlying-integer ordinals below mirror Veng::Renderer::ShaderStage /
        // DescriptorType / Format (Renderer/Types.h) — kept in sync by hand per the
        // cycle-avoidance rule documented in assetformat's CookedBlobs.h.
        constexpr u32 ShaderStageVertex = 1u << 0;
        constexpr u32 ShaderStageFragment = 1u << 1;
        constexpr u32 ShaderStageCompute = 1u << 2;

        constexpr u32 DescriptorTypeCombinedImageSampler = 0;
        constexpr u32 DescriptorTypeSampledImage = 1;
        constexpr u32 DescriptorTypeStorageImage = 2;
        constexpr u32 DescriptorTypeUniformBuffer = 3;
        constexpr u32 DescriptorTypeStorageBuffer = 4;
        constexpr u32 DescriptorTypeSampler = 5;

        // Format ordinals (mirroring Renderer::Format) — used for Slang reflection
        // comparison against VertexLayout element formats.
        constexpr u32 FormatR32Sfloat = 7;
        constexpr u32 FormatRG32Sfloat = 8;
        constexpr u32 FormatRGB32Sfloat = 9;
        constexpr u32 FormatRGBA32Sfloat = 10;

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h);
        // truncate rather than fail on an over-long identifier.
        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        string DiagnosticsText(slang::IBlob* diagnostics)
        {
            if (!diagnostics || diagnostics->getBufferSize() == 0)
                return "";

            return string(static_cast<const char*>(diagnostics->getBufferPointer()), diagnostics->getBufferSize());
        }

        optional<u32> MapShaderStage(SlangStage stage)
        {
            switch (stage)
            {
                case SLANG_STAGE_VERTEX: return ShaderStageVertex;
                case SLANG_STAGE_FRAGMENT: return ShaderStageFragment;
                case SLANG_STAGE_COMPUTE: return ShaderStageCompute;
                default: return std::nullopt;
            }
        }

        // Maps a global shader parameter's reflected binding (Texture2D, SamplerState,
        // Sampler2D, ConstantBuffer<T>, [RW]StructuredBuffer<T>, ...) to a
        // Renderer::DescriptorType ordinal. Read-only vs. read-write resources
        // (SampledImage vs. StorageImage) are distinguished via the leaf type's
        // resource access.
        Result<u32> MapBindingType(slang::TypeLayoutReflection* typeLayout, const string& paramName,
            const path& sourcePath, const string& entryName)
        {
            if (typeLayout->getBindingRangeCount() == 0)
            {
                return std::unexpected(fmt::format(
                    "shader importer: '{}': entry point '{}': parameter '{}' has no binding range",
                    sourcePath.string(), entryName, paramName));
            }

            const slang::BindingType bindingType = typeLayout->getBindingRangeType(0);
            switch (bindingType)
            {
                case slang::BindingType::CombinedTextureSampler:
                    return DescriptorTypeCombinedImageSampler;

                case slang::BindingType::Texture:
                {
                    slang::TypeLayoutReflection* leaf = typeLayout->getBindingRangeLeafTypeLayout(0);
                    const SlangResourceAccess access = leaf->getResourceAccess();
                    return access == SLANG_RESOURCE_ACCESS_READ ? DescriptorTypeSampledImage : DescriptorTypeStorageImage;
                }

                case slang::BindingType::Sampler:
                    return DescriptorTypeSampler;

                case slang::BindingType::ConstantBuffer:
                    return DescriptorTypeUniformBuffer;

                case slang::BindingType::TypedBuffer:
                case slang::BindingType::RawBuffer:
                    return DescriptorTypeStorageBuffer;

                default:
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': parameter '{}' has unsupported binding type {}",
                        sourcePath.string(), entryName, paramName, static_cast<int>(bindingType)));
            }
        }

        // Maps a Slang-reflected vertex input type to a format ordinal.
        // Used only in the Slang source path (for element-for-element validation
        // against the referenced VertexLayout). Only float/floatN are valid.
        Result<u32> MapVertexInputFormat(slang::TypeReflection* type, const string& name,
            const path& sourcePath, const string& entryName)
        {
            const auto unsupported = [&]() -> Result<u32>
            {
                return std::unexpected(fmt::format(
                    "shader importer: '{}': entry point '{}': vertex input '{}' has unsupported type "
                    "(only float/float2/float3/float4 are supported)",
                    sourcePath.string(), entryName, name));
            };

            if (type->getScalarType() != slang::TypeReflection::ScalarType::Float32)
                return unsupported();

            if (type->getKind() == slang::TypeReflection::Kind::Scalar)
                return FormatR32Sfloat;

            if (type->getKind() == slang::TypeReflection::Kind::Vector)
            {
                switch (type->getColumnCount())
                {
                    case 2: return FormatRG32Sfloat;
                    case 3: return FormatRGB32Sfloat;
                    case 4: return FormatRGBA32Sfloat;
                    default: return unsupported();
                }
            }

            return unsupported();
        }

        // Assembles the final cooked blob: CookedShaderHeader, CookedShaderInterfaceHeader,
        // bindings, push constants, then SPIR-V. No vertex-input table — the layout
        // is referenced by AssetId (0 = no vertex inputs).
        vector<u8> BuildBlob(const string& entryPoint, std::span<const u8> spirv,
            const vector<CookedDescriptorBinding>& bindings,
            const vector<CookedPushConstantBlock>& pushConstants,
            u64 vertexLayoutAssetId)
        {
            CookedShaderInterfaceHeader interfaceHeader{};
            interfaceHeader.BindingCount = static_cast<u32>(bindings.size());
            interfaceHeader.PushConstantCount = static_cast<u32>(pushConstants.size());
            interfaceHeader.VertexLayoutAssetId = vertexLayoutAssetId;

            const usize bindingBytes = bindings.size() * sizeof(CookedDescriptorBinding);
            const usize pushConstantBytes = pushConstants.size() * sizeof(CookedPushConstantBlock);
            const usize interfaceBytes = sizeof(CookedShaderInterfaceHeader) + bindingBytes + pushConstantBytes;

            CookedShaderHeader header{};
            SetName(header.EntryPoint, entryPoint);
            header.InterfaceBytes = static_cast<u32>(interfaceBytes);
            header.SpirvBytes = static_cast<u32>(spirv.size());

            vector<u8> blob(sizeof(CookedShaderHeader) + interfaceBytes + spirv.size());
            usize cursor = 0;
            std::memcpy(blob.data() + cursor, &header, sizeof(header));
            cursor += sizeof(header);
            std::memcpy(blob.data() + cursor, &interfaceHeader, sizeof(interfaceHeader));
            cursor += sizeof(interfaceHeader);
            std::memcpy(blob.data() + cursor, bindings.data(), bindingBytes);
            cursor += bindingBytes;
            std::memcpy(blob.data() + cursor, pushConstants.data(), pushConstantBytes);
            cursor += pushConstantBytes;
            std::memcpy(blob.data() + cursor, spirv.data(), spirv.size());

            return blob;
        }

        // Compiles `entryName` from the .slang file at `sourcePath` to SPIR-V and
        // reflects its interface via Slang's own reflection API. If vertex_layout is
        // present (non-zero), reflects the vertex stage's inputs and validates them
        // element-for-element against the referenced layout. If absent, any reflected
        // vertex inputs are discarded (vertex-pulling / no-input semantics).
        Result<vector<u8>> CookFromSource(const CookContext& context, const path& sourcePath,
            const string& entryName, u64 vertexLayoutAssetId)
        {
            ComPtr<slang::IGlobalSession> globalSession;
            if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
                return std::unexpected("shader importer: failed to create Slang global session");

            slang::TargetDesc targetDesc{};
            targetDesc.format = SLANG_SPIRV;
            targetDesc.profile = globalSession->findProfile("spirv_1_5");

            const string searchPath = sourcePath.parent_path().string();
            const char* searchPaths[] = {searchPath.c_str()};

            slang::SessionDesc sessionDesc{};
            sessionDesc.targets = &targetDesc;
            sessionDesc.targetCount = 1;
            sessionDesc.searchPaths = searchPaths;
            sessionDesc.searchPathCount = 1;

            ComPtr<slang::ISession> session;
            if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
                return std::unexpected("shader importer: failed to create Slang session");

            const string moduleName = sourcePath.stem().string();

            ComPtr<slang::IBlob> diagnostics;
            slang::IModule* module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
            if (!module)
            {
                return std::unexpected(fmt::format("shader importer: '{}': failed to compile: {}",
                    sourcePath.string(), DiagnosticsText(diagnostics)));
            }

            ComPtr<slang::IEntryPoint> entryPoint;
            if (SLANG_FAILED(module->findEntryPointByName(entryName.c_str(), entryPoint.writeRef())))
            {
                return std::unexpected(fmt::format(
                    "shader importer: '{}': entry point '{}' not found (missing [shader(\"...\")] attribute?)",
                    sourcePath.string(), entryName));
            }

            slang::IComponentType* components[] = {module, entryPoint};
            ComPtr<slang::IComponentType> program;
            diagnostics = nullptr;
            if (SLANG_FAILED(session->createCompositeComponentType(components, 2, program.writeRef(), diagnostics.writeRef())))
            {
                return std::unexpected(fmt::format("shader importer: '{}': entry point '{}': failed to compose: {}",
                    sourcePath.string(), entryName, DiagnosticsText(diagnostics)));
            }

            ComPtr<slang::IComponentType> linkedProgram;
            diagnostics = nullptr;
            if (SLANG_FAILED(program->link(linkedProgram.writeRef(), diagnostics.writeRef())))
            {
                return std::unexpected(fmt::format("shader importer: '{}': entry point '{}': failed to link: {}",
                    sourcePath.string(), entryName, DiagnosticsText(diagnostics)));
            }

            ComPtr<slang::IBlob> code;
            diagnostics = nullptr;
            if (SLANG_FAILED(linkedProgram->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())))
            {
                return std::unexpected(fmt::format("shader importer: '{}': entry point '{}': codegen failed: {}",
                    sourcePath.string(), entryName, DiagnosticsText(diagnostics)));
            }

            slang::ProgramLayout* layout = linkedProgram->getLayout();
            if (!layout || layout->getEntryPointCount() != 1)
            {
                return std::unexpected(fmt::format("shader importer: '{}': entry point '{}': unexpected reflection layout",
                    sourcePath.string(), entryName));
            }

            slang::EntryPointReflection* entryPointLayout = layout->getEntryPointByIndex(0);
            const optional<u32> stageMask = MapShaderStage(entryPointLayout->getStage());
            if (!stageMask)
            {
                return std::unexpected(fmt::format("shader importer: '{}': entry point '{}' has an unsupported stage",
                    sourcePath.string(), entryName));
            }

            const bool isVertexStage = *stageMask == ShaderStageVertex;

            vector<CookedDescriptorBinding> bindings;
            vector<CookedPushConstantBlock> pushConstants;

            for (unsigned i = 0; i < layout->getParameterCount(); ++i)
            {
                slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);

                if (param->getCategory() == slang::ParameterCategory::PushConstantBuffer)
                {
                    slang::TypeLayoutReflection* elementType = param->getTypeLayout()->getElementTypeLayout();
                    const usize size = elementType ? elementType->getSize(slang::ParameterCategory::Uniform) : 0;
                    if (size == 0 || size > 128)
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: '{}': entry point '{}': push constant '{}' has invalid size {} (must be 1-128 bytes)",
                            sourcePath.string(), entryName, param->getName(), size));
                    }

                    CookedPushConstantBlock block{};
                    block.Offset = static_cast<u32>(param->getOffset(slang::ParameterCategory::PushConstantBuffer));
                    block.Size = static_cast<u32>(size);
                    block.StageMask = *stageMask;
                    SetName(block.Name, param->getName());
                    pushConstants.push_back(block);
                    continue;
                }

                // Set 0 is the bindless registry (plan 05) — recognized and excluded
                // from the declared interface; author bindings live in sets >= 1.
                const u32 set = param->getBindingSpace();
                if (set == 0)
                    continue;

                const Result<u32> descriptorType = MapBindingType(param->getTypeLayout(), param->getName(), sourcePath, entryName);
                if (!descriptorType)
                    return std::unexpected(descriptorType.error());

                CookedDescriptorBinding binding{};
                binding.Set = set;
                binding.Binding = param->getBindingIndex();
                binding.Type = *descriptorType;
                binding.Count = 1;
                binding.StageMask = *stageMask;
                SetName(binding.Name, param->getName());
                bindings.push_back(binding);
            }

            // Reflect vertex inputs and validate against the referenced layout (if any).
            if (isVertexStage && vertexLayoutAssetId != 0)
            {
                // Collect reflected inputs sorted by location.
                struct ReflectedInput { u32 Location; u32 Format; string Name; };
                vector<ReflectedInput> reflectedInputs;

                for (unsigned i = 0; i < entryPointLayout->getParameterCount(); ++i)
                {
                    slang::VariableLayoutReflection* param = entryPointLayout->getParameterByIndex(i);
                    if (param->getCategory() != slang::ParameterCategory::VaryingInput)
                        continue;

                    slang::TypeLayoutReflection* typeLayout = param->getTypeLayout();
                    if (typeLayout->getKind() == slang::TypeReflection::Kind::Struct)
                    {
                        for (unsigned f = 0; f < typeLayout->getFieldCount(); ++f)
                        {
                            slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(f);
                            const Result<u32> format = MapVertexInputFormat(field->getType(), field->getName(), sourcePath, entryName);
                            if (!format)
                                return std::unexpected(format.error());
                            reflectedInputs.push_back({field->getBindingIndex(), *format, field->getName()});
                        }
                    }
                    else
                    {
                        const Result<u32> format = MapVertexInputFormat(param->getType(), param->getName(), sourcePath, entryName);
                        if (!format)
                            return std::unexpected(format.error());
                        reflectedInputs.push_back({param->getBindingIndex(), *format, param->getName()});
                    }
                }

                std::ranges::sort(reflectedInputs, [](const auto& a, const auto& b) { return a.Location < b.Location; });
                for (usize i = 0; i < reflectedInputs.size(); ++i)
                {
                    if (reflectedInputs[i].Location != static_cast<u32>(i))
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: '{}': entry point '{}': vertex input locations are not contiguous from 0 (got {} at index {})",
                            sourcePath.string(), entryName, reflectedInputs[i].Location, i));
                    }
                }

                // Resolve the referenced VertexLayout source.
                if (!context.Resolve)
                {
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': vertex_layout {} specified but no resolver available",
                        sourcePath.string(), entryName, vertexLayoutAssetId));
                }

                const optional<ResolvedSource> resolved = context.Resolve(AssetId{.Value = vertexLayoutAssetId});
                if (!resolved)
                {
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': vertex_layout {} not found in pack or reference packs",
                        sourcePath.string(), entryName, vertexLayoutAssetId));
                }

                if (resolved->Type != AssetType::VertexLayout)
                {
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': asset {} referenced as vertex_layout but has type {}",
                        sourcePath.string(), entryName, vertexLayoutAssetId,
                        static_cast<u32>(resolved->Type)));
                }

                const Result<vector<CookedVertexLayoutElement>> layoutElements =
                    ReadVertexLayoutFile(resolved->AbsolutePath);
                if (!layoutElements)
                {
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': failed to read vertex layout {}: {}",
                        sourcePath.string(), entryName, vertexLayoutAssetId, layoutElements.error()));
                }

                // Validate element-for-element (format in location order; names need not match).
                if (reflectedInputs.size() != layoutElements->size())
                {
                    return std::unexpected(fmt::format(
                        "shader importer: '{}': entry point '{}': reflected {} vertex input(s) but "
                        "vertex_layout {} has {} element(s)",
                        sourcePath.string(), entryName, reflectedInputs.size(),
                        vertexLayoutAssetId, layoutElements->size()));
                }

                for (usize i = 0; i < reflectedInputs.size(); ++i)
                {
                    const u32 reflectedFmt = reflectedInputs[i].Format;
                    const u32 layoutFmt = (*layoutElements)[i].Format;
                    if (reflectedFmt != layoutFmt)
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: '{}': entry point '{}': vertex input[{}] '{}' "
                            "has format ordinal {} but vertex_layout {} element[{}] has format ordinal {}",
                            sourcePath.string(), entryName, i, reflectedInputs[i].Name,
                            reflectedFmt, vertexLayoutAssetId, i, layoutFmt));
                    }
                }
            }
            // If vertex_layout is absent (vertexLayoutAssetId == 0), any reflected
            // vertex inputs are discarded — vertex-pulling / no-input semantics.

            // Slang's SPIR-V backend names every entry point "main" regardless of
            // its source name (confirmed via spirv-dis), so Shader::Create's
            // ShaderBinaryInfo::EntryPoint is always "main" for this path.
            return BuildBlob("main",
                std::span(static_cast<const u8*>(code->getBufferPointer()), code->getBufferSize()),
                bindings, pushConstants, vertexLayoutAssetId);
        }

        optional<u32> ParseDescriptorType(const string& name)
        {
            if (name == "combined_image_sampler") return DescriptorTypeCombinedImageSampler;
            if (name == "sampled_image") return DescriptorTypeSampledImage;
            if (name == "storage_image") return DescriptorTypeStorageImage;
            if (name == "uniform_buffer") return DescriptorTypeUniformBuffer;
            if (name == "storage_buffer") return DescriptorTypeStorageBuffer;
            if (name == "sampler") return DescriptorTypeSampler;
            return std::nullopt;
        }

        optional<u32> ParseShaderStageMask(const json& stages)
        {
            if (!stages.is_array() || stages.empty())
                return std::nullopt;

            u32 mask = 0;
            for (const json& stage : stages)
            {
                if (!stage.is_string())
                    return std::nullopt;

                const string name = stage.get<string>();
                if (name == "vertex") mask |= ShaderStageVertex;
                else if (name == "fragment") mask |= ShaderStageFragment;
                else if (name == "compute") mask |= ShaderStageCompute;
                else return std::nullopt;
            }

            return mask;
        }

        optional<vector<u8>> DecodeBase64(std::string_view text)
        {
            const auto decodeChar = [](char c) -> int
            {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1;
            };

            vector<u8> out;
            out.reserve(text.size() / 4 * 3);

            u32 buffer = 0;
            int bits = 0;
            for (const char c : text)
            {
                if (c == '=' || c == '\n' || c == '\r' || c == ' ')
                    continue;

                const int value = decodeChar(c);
                if (value < 0)
                    return std::nullopt;

                buffer = (buffer << 6) | static_cast<u32>(value);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<u8>((buffer >> bits) & 0xFF));
                }
            }

            return out;
        }

        // The editor/inline path: precompiled SPIR-V (base64) with its ShaderInterface
        // supplied directly. We do NOT reflect/validate vertex inputs from precompiled
        // SPIR-V — the project deliberately adds no SPIRV-Reflect dependency. The
        // vertex_layout field is an existence+type check only: if present, we confirm
        // the asset exists and is AssetType::VertexLayout, then store its id.
        Result<vector<u8>> CookInline(const CookContext& context, const json& entry)
        {
            if (!entry["spirv_b64"].is_string())
                return std::unexpected("shader importer: 'spirv_b64' must be a string");

            const optional<vector<u8>> spirv = DecodeBase64(entry["spirv_b64"].get<string>());
            if (!spirv)
                return std::unexpected("shader importer: 'spirv_b64' is not valid base64");

            if (!entry.contains("entry") || !entry["entry"].is_string())
                return std::unexpected("shader importer: missing or invalid 'entry'");

            if (!entry.contains("interface") || !entry["interface"].is_object())
                return std::unexpected("shader importer: missing or invalid 'interface'");

            const json& interfaceJson = entry["interface"];

            vector<CookedDescriptorBinding> bindings;
            if (interfaceJson.contains("bindings"))
            {
                if (!interfaceJson["bindings"].is_array())
                    return std::unexpected("shader importer: 'interface.bindings' must be an array");

                for (const json& b : interfaceJson["bindings"])
                {
                    if (!b.is_object() || !b.contains("name") || !b["name"].is_string()
                        || !b.contains("set") || !b["set"].is_number_unsigned()
                        || !b.contains("binding") || !b["binding"].is_number_unsigned()
                        || !b.contains("type") || !b["type"].is_string()
                        || !b.contains("stages"))
                    {
                        return std::unexpected("shader importer: invalid entry in 'interface.bindings'");
                    }

                    const u32 set = b["set"].get<u32>();
                    if (set < 1)
                        return std::unexpected("shader importer: 'interface.bindings' entry targets set 0 (reserved for the bindless registry)");

                    const optional<u32> type = ParseDescriptorType(b["type"].get<string>());
                    if (!type)
                    {
                        return std::unexpected(fmt::format("shader importer: 'interface.bindings' entry has unrecognized type '{}'",
                            b["type"].get<string>()));
                    }

                    const optional<u32> stageMask = ParseShaderStageMask(b["stages"]);
                    if (!stageMask)
                        return std::unexpected("shader importer: 'interface.bindings' entry has invalid 'stages'");

                    CookedDescriptorBinding binding{};
                    binding.Set = set;
                    binding.Binding = b["binding"].get<u32>();
                    binding.Type = *type;
                    binding.Count = b.contains("count") && b["count"].is_number_unsigned() ? b["count"].get<u32>() : 1;
                    binding.StageMask = *stageMask;
                    SetName(binding.Name, b["name"].get<string>());
                    bindings.push_back(binding);
                }
            }

            vector<CookedPushConstantBlock> pushConstants;
            if (interfaceJson.contains("push_constants"))
            {
                if (!interfaceJson["push_constants"].is_array())
                    return std::unexpected("shader importer: 'interface.push_constants' must be an array");

                for (const json& p : interfaceJson["push_constants"])
                {
                    if (!p.is_object() || !p.contains("name") || !p["name"].is_string()
                        || !p.contains("offset") || !p["offset"].is_number_unsigned()
                        || !p.contains("size") || !p["size"].is_number_unsigned()
                        || !p.contains("stages"))
                    {
                        return std::unexpected("shader importer: invalid entry in 'interface.push_constants'");
                    }

                    const u32 size = p["size"].get<u32>();
                    if (size == 0 || size > 128)
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: 'interface.push_constants' entry '{}' has invalid size {} (must be 1-128 bytes)",
                            p["name"].get<string>(), size));
                    }

                    const optional<u32> stageMask = ParseShaderStageMask(p["stages"]);
                    if (!stageMask)
                        return std::unexpected("shader importer: 'interface.push_constants' entry has invalid 'stages'");

                    CookedPushConstantBlock block{};
                    block.Offset = p["offset"].get<u32>();
                    block.Size = size;
                    block.StageMask = *stageMask;
                    SetName(block.Name, p["name"].get<string>());
                    pushConstants.push_back(block);
                }
            }

            // Resolve the vertex_layout reference (existence + type check only).
            // We do NOT validate individual elements against the precompiled SPIR-V
            // because the project deliberately adds no SPIRV-Reflect dependency —
            // that validation is only possible on the Slang source path.
            u64 vertexLayoutAssetId = 0;
            if (entry.contains("vertex_layout") && entry["vertex_layout"].is_number_unsigned())
            {
                vertexLayoutAssetId = entry["vertex_layout"].get<u64>();
                if (vertexLayoutAssetId != 0)
                {
                    if (!context.Resolve)
                    {
                        return std::unexpected("shader importer: vertex_layout specified but no resolver available");
                    }

                    const optional<ResolvedSource> resolved =
                        context.Resolve(AssetId{.Value = vertexLayoutAssetId});
                    if (!resolved)
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: vertex_layout {} not found in pack or reference packs",
                            vertexLayoutAssetId));
                    }

                    if (resolved->Type != AssetType::VertexLayout)
                    {
                        return std::unexpected(fmt::format(
                            "shader importer: asset {} referenced as vertex_layout but has type {}",
                            vertexLayoutAssetId, static_cast<u32>(resolved->Type)));
                    }
                }
            }

            return BuildBlob(entry["entry"].get<string>(), *spirv, bindings, pushConstants, vertexLayoutAssetId);
        }
    }

    Result<vector<u8>> ShaderImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (entry.contains("spirv_b64"))
            return CookInline(context, entry);

        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("shader importer: missing or invalid 'source'");

        if (!entry.contains("entry") || !entry["entry"].is_string())
            return std::unexpected("shader importer: missing or invalid 'entry'");

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        u64 vertexLayoutAssetId = 0;
        if (entry.contains("vertex_layout") && entry["vertex_layout"].is_number_unsigned())
            vertexLayoutAssetId = entry["vertex_layout"].get<u64>();

        return CookFromSource(context, sourcePath, entry["entry"].get<string>(), vertexLayoutAssetId);
    }
}
