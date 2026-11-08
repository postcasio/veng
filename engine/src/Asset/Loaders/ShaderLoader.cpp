#include "ShaderLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/CookedShader.h>
#include <Veng/Cook/MetalBindingSlots.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Asset/VertexLayout.h>

namespace Veng
{
    namespace
    {
        // The Bridge* helpers below map the cooked blob's underlying-integer
        // enum fields to their Veng::Renderer enums — the engine side of the
        // cycle-avoidance rule documented in assetpack's CookedBlobs.h. An
        // unrecognized value means a stale/corrupt cooked archive, hence
        // AssetError::Corrupt (recoverable) rather than VE_ASSERT.

        optional<Renderer::DescriptorType> BridgeDescriptorType(u32 value)
        {
            switch (value)
            {
            case static_cast<u32>(Renderer::DescriptorType::CombinedImageSampler):
                return Renderer::DescriptorType::CombinedImageSampler;
            case static_cast<u32>(Renderer::DescriptorType::SampledImage):
                return Renderer::DescriptorType::SampledImage;
            case static_cast<u32>(Renderer::DescriptorType::StorageImage):
                return Renderer::DescriptorType::StorageImage;
            case static_cast<u32>(Renderer::DescriptorType::UniformBuffer):
                return Renderer::DescriptorType::UniformBuffer;
            case static_cast<u32>(Renderer::DescriptorType::StorageBuffer):
                return Renderer::DescriptorType::StorageBuffer;
            case static_cast<u32>(Renderer::DescriptorType::Sampler):
                return Renderer::DescriptorType::Sampler;
            default:
                return std::nullopt;
            }
        }

        optional<Renderer::ShaderStage> BridgeShaderStageMask(u32 value)
        {
            constexpr u32 KnownStages = static_cast<u32>(Renderer::ShaderStage::Vertex) |
                                        static_cast<u32>(Renderer::ShaderStage::Fragment) |
                                        static_cast<u32>(Renderer::ShaderStage::Compute);

            if (value == 0 || (value & ~KnownStages) != 0)
            {
                return std::nullopt;
            }

            return static_cast<Renderer::ShaderStage>(value);
        }

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h).
        template <usize N>
        string BridgeName(const char (&name)[N])
        {
            return string(name, strnlen(name, N));
        }

        // The section and module kind of the backend compiled in.
#if defined(VE_GPU_BACKEND_VULKAN)
        constexpr CookedShaderCode NativeShaderCode = CookedShaderCode::SpirV;
        constexpr Renderer::ShaderCodeKind NativeShaderCodeKind = Renderer::ShaderCodeKind::SpirV;
#elif defined(VE_GPU_BACKEND_METAL)
        constexpr CookedShaderCode NativeShaderCode = CookedShaderCode::MetalLibrary;
        constexpr Renderer::ShaderCodeKind NativeShaderCodeKind =
            Renderer::ShaderCodeKind::MetalLibrary;
#endif

        // A Metal binding names an argument-buffer element, or a buffer slot, which must be one of
        // the reserved dynamic-uniform-buffer slots.
        optional<Renderer::MetalBindingKind> BridgeMetalBinding(u32 kind, u32 index)
        {
            using namespace Cook::MetalBindingSlots;
            switch (kind)
            {
            case static_cast<u32>(CookedMetalBinding::None):
                return Renderer::MetalBindingKind::None;
            case static_cast<u32>(CookedMetalBinding::ArgumentBufferElement):
                return Renderer::MetalBindingKind::ArgumentBufferElement;
            case static_cast<u32>(CookedMetalBinding::BufferSlot):
                if (index < DynamicUniformBufferBase ||
                    index >= DynamicUniformBufferBase + MaxDynamicUniformBuffers)
                {
                    return std::nullopt;
                }
                return Renderer::MetalBindingKind::BufferSlot;
            default:
                return std::nullopt;
            }
        }

        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::ParsedAsset> ShaderLoader::Parse(const AssetParseContext& /*context*/,
                                                         const AssetId id,
                                                         const std::span<const u8> cooked) const
    {
        const Result<CookedShaderView> parsedBlob = ParseCookedShader(cooked);
        if (!parsedBlob)
        {
            return std::unexpected(Corrupt(id, fmt::format("shader: {}", parsedBlob.error())));
        }
        const CookedShaderView& view = *parsedBlob;

        const Result<const CookedShaderSectionView*> section =
            FindCookedShaderSection(view, NativeShaderCode, fmt::format("{:#018x}", id.Value));
        if (!section)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Unsupported, .Id = id, .Detail = section.error()});
        }

        Renderer::ShaderInterface shaderInterface;
        shaderInterface.Bindings.reserve(view.Bindings.size());
        for (usize i = 0; i < view.Bindings.size(); ++i)
        {
            const CookedDescriptorBinding& binding = view.Bindings[i];
            const optional<Renderer::DescriptorType> type = BridgeDescriptorType(binding.Type);
            const optional<Renderer::ShaderStage> stages = BridgeShaderStageMask(binding.StageMask);
            const optional<Renderer::MetalBindingKind> metal =
                BridgeMetalBinding(binding.MetalBinding, binding.MetalIndex);
            if (!type || !stages || !metal)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("shader: descriptor binding {} has unrecognized type {}, stage "
                                    "mask {} or Metal binding {} at {}",
                                    i, binding.Type, binding.StageMask, binding.MetalBinding,
                                    binding.MetalIndex)));
            }

            if (binding.Set < Renderer::BindlessRegistry::FirstUserSet)
            {
                return std::unexpected(
                    Corrupt(id, fmt::format("shader: descriptor binding {} targets set {}, one of "
                                            "the bindless registry's",
                                            i, binding.Set)));
            }

            shaderInterface.Bindings.push_back(Renderer::ShaderBinding{
                .Name = BridgeName(binding.Name),
                .Set = binding.Set,
                .Binding = binding.Binding,
                .Type = *type,
                .Count = binding.Count,
                .Stages = *stages,
                .MetalKind = *metal,
                .MetalIndex = binding.MetalIndex,
            });
        }

        shaderInterface.PushConstants.reserve(view.PushConstants.size());
        for (usize i = 0; i < view.PushConstants.size(); ++i)
        {
            const CookedPushConstantBlock& pushConstant = view.PushConstants[i];
            const optional<Renderer::ShaderStage> stages =
                BridgeShaderStageMask(pushConstant.StageMask);
            if (!stages)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("shader: push constant {} has unrecognized stage mask {}", i,
                                    pushConstant.StageMask)));
            }

            shaderInterface.PushConstants.push_back(Renderer::ShaderPushConstant{
                .Name = BridgeName(pushConstant.Name),
                .Offset = pushConstant.Offset,
                .Size = pushConstant.Size,
                .Stages = *stages,
            });
        }

        const CookedShaderInterfaceHeader& interfaceHeader = view.Interface;
        shaderInterface.VertexLayoutId =
            interfaceHeader.VertexLayoutAssetId != 0
                ? optional<AssetId>(AssetId{interfaceHeader.VertexLayoutAssetId})
                : std::nullopt;

        // A missing vertex layout fails the load — catches a missing or corrupt core pack. The
        // shader waits for it, so a material building its pipeline finds it resident.
        Detail::ParsedAsset parsed;
        if (interfaceHeader.VertexLayoutAssetId != 0)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::VertexLayout,
                                           .Id = AssetId{interfaceHeader.VertexLayoutAssetId}});
        }

        // The module itself is created on the main thread; the code is copied out of the blob for
        // it, since the completion may run after the archive is gone.
        const auto code = CreateRef<vector<u8>>((*section)->Bytes.begin(), (*section)->Bytes.end());
        const auto parsedInterface =
            CreateRef<Renderer::ShaderInterface>(std::move(shaderInterface));
        const uvec3 threadgroupSize{view.Interface.ThreadgroupSize[0],
                                    view.Interface.ThreadgroupSize[1],
                                    view.Interface.ThreadgroupSize[2]};
        parsed.Complete =
            [code, parsedInterface, entryPoint = (*section)->EntryPoint, threadgroupSize,
             id](AssetManager& manager, std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            const Ref<Renderer::ShaderModule> shader = Renderer::ShaderModule::Create(
                manager.GetContext(), {
                                          .Name = fmt::format("Shader {}", id.Value),
                                          .Binary = *code,
                                          .Kind = NativeShaderCodeKind,
                                          .EntryPoint = entryPoint,
                                          .ThreadgroupSize = threadgroupSize,
                                      });

            const Ref<Veng::Shader> asset = CreateRef<Veng::Shader>(Veng::Shader{
                .Module = shader,
                .Interface = std::move(*parsedInterface),
            });

            // The empty finalize is what holds the shader pending until its vertex layout is
            // resident.
            return Detail::LoadJob{
                .Resource = Detail::RefAny(asset),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
                .Finalize = []() -> VoidResult { return {}; },
            };
        };
        return parsed;
    }
}
