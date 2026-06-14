#include "ShaderLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Renderer/VertexLayoutAsset.h>

namespace Veng
{
    namespace
    {
        // The Bridge* helpers below map the cooked blob's underlying-integer
        // enum fields to their Veng::Renderer enums — the engine side of the
        // cycle-avoidance rule documented in assetformat's CookedBlobs.h. An
        // unrecognized value means a stale/corrupt cooked archive, hence
        // AssetError::Corrupt (recoverable) rather than VE_ASSERT.

        optional<Renderer::DescriptorType> BridgeDescriptorType(u32 value)
        {
            switch (value)
            {
                case static_cast<u32>(Renderer::DescriptorType::CombinedImageSampler): return Renderer::DescriptorType::CombinedImageSampler;
                case static_cast<u32>(Renderer::DescriptorType::SampledImage): return Renderer::DescriptorType::SampledImage;
                case static_cast<u32>(Renderer::DescriptorType::StorageImage): return Renderer::DescriptorType::StorageImage;
                case static_cast<u32>(Renderer::DescriptorType::UniformBuffer): return Renderer::DescriptorType::UniformBuffer;
                case static_cast<u32>(Renderer::DescriptorType::StorageBuffer): return Renderer::DescriptorType::StorageBuffer;
                case static_cast<u32>(Renderer::DescriptorType::Sampler): return Renderer::DescriptorType::Sampler;
                default: return std::nullopt;
            }
        }

        optional<Renderer::ShaderStage> BridgeShaderStageMask(u32 value)
        {
            constexpr u32 k_KnownStages = static_cast<u32>(Renderer::ShaderStage::Vertex)
                | static_cast<u32>(Renderer::ShaderStage::Fragment)
                | static_cast<u32>(Renderer::ShaderStage::Compute);

            if (value == 0 || (value & ~k_KnownStages) != 0)
                return std::nullopt;

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
            return AssetLoadError{.Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::RefAny> ShaderLoader::Load(
        AssetManager& manager, Renderer::Context& context,
        AssetId id, std::span<const u8> cooked) const
    {
        if (cooked.size() < sizeof(CookedShaderHeader))
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than CookedShaderHeader"));

        CookedShaderHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        usize cursor = sizeof(CookedShaderHeader);

        if (cooked.size() < cursor + sizeof(CookedShaderInterfaceHeader))
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than CookedShaderInterfaceHeader"));

        CookedShaderInterfaceHeader interfaceHeader;
        std::memcpy(&interfaceHeader, cooked.data() + cursor, sizeof(interfaceHeader));
        cursor += sizeof(CookedShaderInterfaceHeader);

        Renderer::ShaderInterface shaderInterface;

        const usize bindingBytes = static_cast<usize>(interfaceHeader.BindingCount) * sizeof(CookedDescriptorBinding);
        if (cooked.size() < cursor + bindingBytes)
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than descriptor binding table"));

        shaderInterface.Bindings.reserve(interfaceHeader.BindingCount);
        for (u32 i = 0; i < interfaceHeader.BindingCount; ++i)
        {
            CookedDescriptorBinding binding;
            std::memcpy(&binding, cooked.data() + cursor + i * sizeof(CookedDescriptorBinding), sizeof(binding));

            const optional<Renderer::DescriptorType> type = BridgeDescriptorType(binding.Type);
            const optional<Renderer::ShaderStage> stages = BridgeShaderStageMask(binding.StageMask);
            if (!type || !stages)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "shader: descriptor binding {} has unrecognized type {} or stage mask {}",
                    i, binding.Type, binding.StageMask)));
            }

            if (binding.Set < 1)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "shader: descriptor binding {} targets set {} (set 0 is reserved for the bindless registry)",
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

        const usize pushConstantBytes = static_cast<usize>(interfaceHeader.PushConstantCount) * sizeof(CookedPushConstantBlock);
        if (cooked.size() < cursor + pushConstantBytes)
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than push-constant table"));

        shaderInterface.PushConstants.reserve(interfaceHeader.PushConstantCount);
        for (u32 i = 0; i < interfaceHeader.PushConstantCount; ++i)
        {
            CookedPushConstantBlock pushConstant;
            std::memcpy(&pushConstant, cooked.data() + cursor + i * sizeof(CookedPushConstantBlock), sizeof(pushConstant));

            const optional<Renderer::ShaderStage> stages = BridgeShaderStageMask(pushConstant.StageMask);
            if (!stages)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "shader: push constant {} has unrecognized stage mask {}", i, pushConstant.StageMask)));
            }

            shaderInterface.PushConstants.push_back(Renderer::ShaderPushConstant{
                .Name = BridgeName(pushConstant.Name),
                .Offset = pushConstant.Offset,
                .Size = pushConstant.Size,
                .Stages = *stages,
            });
        }
        cursor += pushConstantBytes;

        shaderInterface.VertexLayoutId = interfaceHeader.VertexLayoutAssetId != 0
            ? optional<AssetId>(AssetId{interfaceHeader.VertexLayoutAssetId})
            : std::nullopt;

        const usize expectedInterfaceBytes = cursor - sizeof(CookedShaderHeader);
        if (header.InterfaceBytes != expectedInterfaceBytes)
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "shader: InterfaceBytes {} does not match reflected interface size {}",
                header.InterfaceBytes, expectedInterfaceBytes)));
        }

        // If the shader references a vertex layout, load it now to confirm it
        // exists and is valid — a missing layout is a fatal load error (catches
        // corrupted blobs or a missing core pack). Per-attribute validation
        // already happened at cook time; the loader only needs to assert the
        // asset is resolvable.
        if (interfaceHeader.VertexLayoutAssetId != 0)
        {
            const AssetResult<AssetHandle<Renderer::VertexLayoutAsset>> layout =
                manager.LoadSync<Renderer::VertexLayoutAsset>(AssetId{interfaceHeader.VertexLayoutAssetId});
            if (!layout)
                return std::unexpected(layout.error());
        }

        if (cooked.size() < cursor + header.SpirvBytes)
            return std::unexpected(Corrupt(id, "shader: cooked blob smaller than header + SPIR-V"));

        const std::span<const u8> spirv = cooked.subspan(cursor, header.SpirvBytes);

        const Ref<Renderer::Shader> shader = Renderer::Shader::Create(context, {
            .Name = fmt::format("Shader {}", id.Value),
            .Binary = spirv,
            .EntryPoint = BridgeName(header.EntryPoint),
        });

        const Ref<Renderer::ShaderAsset> asset = CreateRef<Renderer::ShaderAsset>(Renderer::ShaderAsset{
            .Module = shader,
            .Interface = std::move(shaderInterface),
        });

        return Detail::RefAny(asset);
    }
}
