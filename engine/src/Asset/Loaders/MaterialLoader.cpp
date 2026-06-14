#include "MaterialLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderAsset.h>
#include <Veng/Renderer/ShaderInterface.h>
#include <Veng/Renderer/Texture.h>
#include <Veng/Renderer/VertexLayoutAsset.h>

namespace Veng
{
    namespace
    {
        // Fixed push-constant offset where the per-draw materialIndex selector
        // lives within the forward push-constant block (right after the float4x4
        // MVP at offset 0, size 64).
        static constexpr u32 MaterialSelectorPushOffset = 64;

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

    AssetResult<Detail::RefAny> MaterialLoader::Load(
        AssetManager& manager, Renderer::Context& context,
        AssetId id, std::span<const u8> cooked) const
    {
        // ── 1. CookedMaterialHeader ──────────────────────────────────────────
        if (cooked.size() < sizeof(CookedMaterialHeader))
            return std::unexpected(Corrupt(id, "material: cooked blob smaller than CookedMaterialHeader"));

        CookedMaterialHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        usize cursor = sizeof(CookedMaterialHeader);

        // Loud cross-language drift guard: sizeof(MaterialData) must equal
        // ParamBytes in the blob. Any drift means the shader and engine
        // disagree on the material layout — a Corrupt error, not a VE_ASSERT,
        // so the user gets a clear load failure rather than a crash.
        if (header.ParamBytes != sizeof(Renderer::MaterialData))
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "material: ParamBytes {} does not match sizeof(MaterialData) {} — "
                "shader/engine MaterialData layout has drifted",
                header.ParamBytes, sizeof(Renderer::MaterialData))));
        }

        // ── 2. CookedMaterialField table ─────────────────────────────────────
        const usize fieldBytes = static_cast<usize>(header.FieldCount) * sizeof(CookedMaterialField);
        if (cooked.size() < cursor + fieldBytes)
            return std::unexpected(Corrupt(id, "material: cooked blob smaller than CookedMaterialField table"));

        vector<CookedMaterialField> cookedFields(header.FieldCount);
        for (u32 i = 0; i < header.FieldCount; ++i)
        {
            std::memcpy(&cookedFields[i],
                        cooked.data() + cursor + i * sizeof(CookedMaterialField),
                        sizeof(CookedMaterialField));
        }
        cursor += fieldBytes;

        // ── 3. Packed param block ─────────────────────────────────────────────
        if (cooked.size() < cursor + header.ParamBytes)
            return std::unexpected(Corrupt(id, "material: cooked blob smaller than param block"));

        Renderer::MaterialData params{};
        std::memcpy(&params, cooked.data() + cursor, header.ParamBytes);
        cursor += header.ParamBytes;

        // ── 4. Load vertex and fragment shader assets ─────────────────────────
        const AssetResult<AssetHandle<Renderer::ShaderAsset>> vsResult =
            manager.LoadSync<Renderer::ShaderAsset>(AssetId{header.VertexShaderId});
        if (!vsResult)
            return std::unexpected(vsResult.error());

        const AssetResult<AssetHandle<Renderer::ShaderAsset>> fsResult =
            manager.LoadSync<Renderer::ShaderAsset>(AssetId{header.FragmentShaderId});
        if (!fsResult)
            return std::unexpected(fsResult.error());

        const AssetHandle<Renderer::ShaderAsset>& vsHandle = *vsResult;
        const AssetHandle<Renderer::ShaderAsset>& fsHandle = *fsResult;

        const Renderer::ShaderAsset& vsAsset = *vsHandle.Get();
        const Renderer::ShaderAsset& fsAsset = *fsHandle.Get();

        // ── 5. Build MaterialField table + resolve textures ───────────────────
        vector<Renderer::MaterialField> fields;
        fields.reserve(header.FieldCount);

        // Track textures by AssetId to deduplicate.
        vector<u64> textureIds;
        vector<AssetHandle<Renderer::Texture>> textures;

        auto byte_ptr = [&params]() { return reinterpret_cast<std::byte*>(&params); };

        for (u32 i = 0; i < header.FieldCount; ++i)
        {
            const CookedMaterialField& cf = cookedFields[i];

            Renderer::MaterialField::FieldKind kind;
            switch (cf.Kind)
            {
                case 0: kind = Renderer::MaterialField::FieldKind::Param;          break;
                case 1: kind = Renderer::MaterialField::FieldKind::TextureHandle;  break;
                case 2: kind = Renderer::MaterialField::FieldKind::SamplerHandle;  break;
                default:
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' has unrecognized Kind {}",
                        i, BridgeName(cf.Name), cf.Kind)));
            }

            fields.push_back(Renderer::MaterialField{
                .Name   = BridgeName(cf.Name),
                .Offset = cf.Offset,
                .Size   = cf.Size,
                .Kind   = kind,
            });

            if (kind == Renderer::MaterialField::FieldKind::TextureHandle
             || kind == Renderer::MaterialField::FieldKind::SamplerHandle)
            {
                if (cf.TextureId == 0)
                {
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' is a handle field but TextureId is 0",
                        i, BridgeName(cf.Name))));
                }

                // Load (or reuse) the texture asset.
                AssetHandle<Renderer::Texture>* texHandle = nullptr;
                for (usize j = 0; j < textureIds.size(); ++j)
                {
                    if (textureIds[j] == cf.TextureId)
                    {
                        texHandle = &textures[j];
                        break;
                    }
                }

                if (!texHandle)
                {
                    const AssetResult<AssetHandle<Renderer::Texture>> texResult =
                        manager.LoadSync<Renderer::Texture>(AssetId{cf.TextureId});
                    if (!texResult)
                        return std::unexpected(texResult.error());

                    textureIds.push_back(cf.TextureId);
                    textures.push_back(*texResult);
                    texHandle = &textures.back();
                }

                // Patch the runtime bindless handle index into params at the
                // field's byte offset.
                const Renderer::Texture& tex = *texHandle->Get();
                u32 handleIndex;
                if (kind == Renderer::MaterialField::FieldKind::TextureHandle)
                    handleIndex = tex.GetHandle().Index;
                else
                    handleIndex = tex.GetSamplerHandle().Index;

                if (cf.Offset + sizeof(u32) > sizeof(Renderer::MaterialData))
                {
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' offset {} + 4 exceeds MaterialData size {}",
                        i, BridgeName(cf.Name), cf.Offset, sizeof(Renderer::MaterialData))));
                }
                std::memcpy(byte_ptr() + cf.Offset, &handleIndex, sizeof(u32));
            }
        }

        // ── 6. Build pipeline ─────────────────────────────────────────────────
        const Renderer::ShaderInterface& vsInterface = vsAsset.Interface;
        const Renderer::ShaderInterface& fsInterface = fsAsset.Interface;

        // Descriptor set layouts: set 0 (registry) then any sets >= 1 from
        // fragment + vertex reflection (forward shaders typically declare none).
        vector<Ref<Renderer::DescriptorSetLayout>> descLayouts;
        descLayouts.push_back(context.GetBindlessRegistry().GetSet0Layout());
        {
            const vector<Ref<Renderer::DescriptorSetLayout>> fsLayouts =
                fsInterface.BuildDescriptorSetLayouts(context, fmt::format("Material{}", id.Value));
            for (auto& l : fsLayouts)
                descLayouts.push_back(l);

            const vector<Ref<Renderer::DescriptorSetLayout>> vsLayouts =
                vsInterface.BuildDescriptorSetLayouts(context, fmt::format("Material{}", id.Value));
            for (auto& l : vsLayouts)
                descLayouts.push_back(l);
        }

        // Push-constant ranges: merge ranges with identical (Offset, Size) by
        // OR-ing Stages. The forward push-constant block (MVP + materialIndex)
        // is declared in both vertex and fragment stages; without merging, two
        // ranges cover the same bytes and CommandBuffer::PushConstants<T>
        // asserts ambiguity. After the merge there is exactly one range with
        // Stages = Vertex|Fragment.
        vector<Renderer::PushConstantRange> mergedRanges;
        auto mergeRange = [&](const Renderer::PushConstantRange& incoming)
        {
            for (auto& existing : mergedRanges)
            {
                if (existing.Offset == incoming.Offset && existing.Size == incoming.Size)
                {
                    existing.Stages = existing.Stages | incoming.Stages;
                    return;
                }
            }
            mergedRanges.push_back(incoming);
        };

        for (const auto& r : vsInterface.BuildPushConstantRanges())
            mergeRange(r);
        for (const auto& r : fsInterface.BuildPushConstantRanges())
            mergeRange(r);

        // Validate that the merged ranges cover [MaterialSelectorPushOffset,
        // MaterialSelectorPushOffset + 4). This is the materialIndex field the
        // forward shader must declare — a missing range means the shader doesn't
        // follow the forward push-constant convention.
        {
            bool selectorCovered = false;
            for (const auto& r : mergedRanges)
            {
                if (r.Offset <= MaterialSelectorPushOffset
                 && r.Offset + r.Size >= MaterialSelectorPushOffset + sizeof(u32))
                {
                    selectorCovered = true;
                    break;
                }
            }
            if (!selectorCovered)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "material: no merged push-constant range covers [{}+4) — "
                    "the shader does not declare the expected forward materialIndex selector",
                    MaterialSelectorPushOffset)));
            }
        }

        const Ref<Renderer::PipelineLayout> pipelineLayout = Renderer::PipelineLayout::Create(context, {
            .Name                = fmt::format("Material {} Layout", id.Value),
            .DescriptorSetLayouts = descLayouts,
            .PushConstantRanges  = mergedRanges,
        });

        // Vertex layout: resolve from the vertex shader's declared VertexLayoutId.
        optional<Renderer::VertexBufferLayout> vertexBufferLayout;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Renderer::VertexLayoutAsset>> layoutResult =
                manager.LoadSync<Renderer::VertexLayoutAsset>(*vsInterface.VertexLayoutId);
            if (!layoutResult)
                return std::unexpected(layoutResult.error());

            vertexBufferLayout = layoutResult->Get()->GetLayout();
        }

        const Ref<Renderer::GraphicsPipeline> pipeline = Renderer::GraphicsPipeline::Create(context, {
            .Name = fmt::format("Material {} Pipeline", id.Value),
            .ColorAttachments = {{.Format = context.GetOutputFormat()}},
            .DepthAttachmentFormat = context.GetDepthFormat(),
            .VertexBufferLayout = vertexBufferLayout,
            .PipelineLayout = pipelineLayout,
            .ShaderStages = {
                {.Stage = Renderer::ShaderStage::Vertex,   .Module = vsAsset.Module},
                {.Stage = Renderer::ShaderStage::Fragment, .Module = fsAsset.Module},
            },
            .CullMode = Renderer::CullMode::Back,
            .DepthTestEnable = true,
            .DepthWriteEnable = true,
        });

        // ── 7. Assemble MaterialInfo and construct Material ───────────────────
        const Renderer::MaterialInfo info{
            .Name           = fmt::format("Material {}", id.Value),
            .Context        = &context,
            .Pipeline       = pipeline,
            .VertexShader   = vsHandle,
            .FragmentShader = fsHandle,
            .Textures       = std::move(textures),
            .Params         = params,
            .Fields         = std::move(fields),
            .SelectorOffset = MaterialSelectorPushOffset,
        };

        const Ref<Renderer::Material> material = Renderer::Material::Create(info);
        return Detail::RefAny(material);
    }
}
