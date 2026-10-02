#include "ShaderLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
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
        if (cooked.size() < sizeof(CookedShaderHeader))
        {
            return std::unexpected(
                Corrupt(id, "shader: cooked blob smaller than CookedShaderHeader"));
        }

        CookedShaderHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        usize cursor = sizeof(CookedShaderHeader);

        if (cooked.size() < cursor + sizeof(CookedShaderInterfaceHeader))
        {
            return std::unexpected(
                Corrupt(id, "shader: cooked blob smaller than CookedShaderInterfaceHeader"));
        }

        CookedShaderInterfaceHeader interfaceHeader;
        std::memcpy(&interfaceHeader, cooked.data() + cursor, sizeof(interfaceHeader));
        cursor += sizeof(CookedShaderInterfaceHeader);

        Renderer::ShaderInterface shaderInterface;

        const usize bindingBytes =
            static_cast<usize>(interfaceHeader.BindingCount) * sizeof(CookedDescriptorBinding);
        if (cooked.size() < cursor + bindingBytes)
        {
            return std::unexpected(
                Corrupt(id, "shader: cooked blob smaller than descriptor binding table"));
        }

        shaderInterface.Bindings.reserve(interfaceHeader.BindingCount);
        for (u32 i = 0; i < interfaceHeader.BindingCount; ++i)
        {
            CookedDescriptorBinding binding;
            std::memcpy(&binding, cooked.data() + cursor + i * sizeof(CookedDescriptorBinding),
                        sizeof(binding));

            const optional<Renderer::DescriptorType> type = BridgeDescriptorType(binding.Type);
            const optional<Renderer::ShaderStage> stages = BridgeShaderStageMask(binding.StageMask);
            if (!type || !stages)
            {
                return std::unexpected(Corrupt(
                    id,
                    fmt::format(
                        "shader: descriptor binding {} has unrecognized type {} or stage mask {}",
                        i, binding.Type, binding.StageMask)));
            }

            if (binding.Set < 1)
            {
                return std::unexpected(
                    Corrupt(id, fmt::format("shader: descriptor binding {} targets set {} (set 0 "
                                            "is reserved for the bindless registry)",
                                            i, binding.Set)));
            }

            shaderInterface.Bindings.push_back(Renderer::ShaderBinding{
                .Name = BridgeName(binding.Name),
                .Set = binding.Set,
                .Binding = binding.Binding,
                .Type = *type,
                .Count = binding.Count,
                .Stages = *stages,
            });
        }
        cursor += bindingBytes;

        const usize pushConstantBytes =
            static_cast<usize>(interfaceHeader.PushConstantCount) * sizeof(CookedPushConstantBlock);
        if (cooked.size() < cursor + pushConstantBytes)
        {
            return std::unexpected(
                Corrupt(id, "shader: cooked blob smaller than push-constant table"));
        }

        shaderInterface.PushConstants.reserve(interfaceHeader.PushConstantCount);
        for (u32 i = 0; i < interfaceHeader.PushConstantCount; ++i)
        {
            CookedPushConstantBlock pushConstant;
            std::memcpy(&pushConstant, cooked.data() + cursor + i * sizeof(CookedPushConstantBlock),
                        sizeof(pushConstant));

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
        cursor += pushConstantBytes;

        shaderInterface.VertexLayoutId =
            interfaceHeader.VertexLayoutAssetId != 0
                ? optional<AssetId>(AssetId{interfaceHeader.VertexLayoutAssetId})
                : std::nullopt;

        const usize expectedInterfaceBytes = cursor - sizeof(CookedShaderHeader);
        if (header.InterfaceBytes != expectedInterfaceBytes)
        {
            return std::unexpected(Corrupt(
                id,
                fmt::format("shader: InterfaceBytes {} does not match reflected interface size {}",
                            header.InterfaceBytes, expectedInterfaceBytes)));
        }

        if (cooked.size() < cursor + header.SpirvBytes)
        {
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than header + SPIR-V"));
        }

        // A missing vertex layout fails the load — catches a missing or corrupt core pack. The
        // shader waits for it, so a material building its pipeline finds it resident.
        Detail::ParsedAsset parsed;
        if (interfaceHeader.VertexLayoutAssetId != 0)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::VertexLayout,
                                           .Id = AssetId{interfaceHeader.VertexLayoutAssetId}});
        }

        // The module itself is created on the main thread; the SPIR-V is copied out of the blob for
        // it, since the completion may run after the archive is gone.
        auto spirv = CreateRef<vector<u8>>(
            cooked.begin() + static_cast<std::ptrdiff_t>(cursor),
            cooked.begin() + static_cast<std::ptrdiff_t>(cursor + header.SpirvBytes));
        auto parsedInterface = CreateRef<Renderer::ShaderInterface>(std::move(shaderInterface));
        parsed.Complete = [spirv, parsedInterface, entryPoint = BridgeName(header.EntryPoint),
                           id](AssetManager& manager,
                               std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            const Ref<Renderer::ShaderModule> shader = Renderer::ShaderModule::Create(
                manager.GetContext(), {
                                          .Name = fmt::format("Shader {}", id.Value),
                                          .Binary = *spirv,
                                          .EntryPoint = entryPoint,
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
