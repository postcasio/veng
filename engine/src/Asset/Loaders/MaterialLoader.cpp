#include "MaterialLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/ShaderInterface.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Asset/VertexLayout.h>

namespace Veng
{
    namespace
    {
        // The push-constant offset of the per-draw materialIndex selector, keyed
        // on domain. A Surface push block is MVP (offset 0, 64 bytes) then the
        // selector, so the selector sits at 64. A PostProcess shader has no
        // geometry block, so its selector sits at 0 — a 4-byte push range, no dead
        // MVP padding. The pipeline-layout guard checks the domain's offset.
        constexpr u32 SurfaceSelectorPushOffset = 64;
        constexpr u32 PostProcessSelectorPushOffset = 0;

        u32 SelectorPushOffsetFor(MaterialDomain domain)
        {
            switch (domain)
            {
                case MaterialDomain::Surface:     return SurfaceSelectorPushOffset;
                case MaterialDomain::PostProcess: return PostProcessSelectorPushOffset;
            }
            VE_ASSERT(false, "MaterialLoader: unmapped MaterialDomain {}", static_cast<u32>(domain));
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

    namespace
    {
        // Build the material's pipeline layout from the (now-resident)
        // vertex/fragment shader interfaces — set 0 reserved for the bindless
        // registry, author-declared sets shifted to 1+, the merged push-constant
        // ranges. A drifted shader (no selector push-constant range at the
        // domain's offset) is a recoverable Corrupt failure.
        Result<Ref<Renderer::PipelineLayout>> BuildPipelineLayout(
            Renderer::Context& context, AssetId id, MaterialDomain domain,
            const Veng::Shader& vsAsset, const Veng::Shader& fsAsset)
        {
            const Renderer::ShaderInterface& vsInterface = vsAsset.Interface;
            const Renderer::ShaderInterface& fsInterface = fsAsset.Interface;

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

            // Merge push-constant ranges with identical (Offset, Size) by OR-ing Stages.
            // A Surface push block is declared in both stages; without merging, two ranges
            // cover the same bytes and CommandBuffer::PushConstants asserts ambiguity.
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

            // The selector must be covered by a declared push range, at the
            // domain's offset (Surface → 64, PostProcess → 0).
            const u32 selectorOffset = SelectorPushOffsetFor(domain);
            bool selectorCovered = false;
            for (const auto& r : mergedRanges)
            {
                if (r.Offset <= selectorOffset
                 && r.Offset + r.Size >= selectorOffset + sizeof(u32))
                {
                    selectorCovered = true;
                    break;
                }
            }
            if (!selectorCovered)
            {
                return std::unexpected(fmt::format(
                    "material: no merged push-constant range covers [{}+4) — "
                    "the shader does not declare the expected materialIndex selector",
                    selectorOffset));
            }

            return Renderer::PipelineLayout::Create(context, {
                .Name                = fmt::format("Material {} Layout", id.Value),
                .DescriptorSetLayouts = descLayouts,
                .PushConstantRanges  = mergedRanges,
            });
        }

        // Build a Surface material's graphics pipeline against the fixed deferred g-buffer formats.
        // Called from the main-thread finalize, where the shaders are guaranteed resident.
        Result<Ref<Renderer::GraphicsPipeline>> BuildSurfacePipeline(
            AssetManager& manager, Renderer::Context& context, AssetId id,
            const Ref<Renderer::PipelineLayout>& layout,
            const Veng::Shader& vsAsset, const Veng::Shader& fsAsset)
        {
            const Renderer::ShaderInterface& vsInterface = vsAsset.Interface;

            // VertexLayout loads are CPU-only; synchronous here is free even on the async path.
            optional<Renderer::VertexBufferLayout> vertexBufferLayout;
            if (vsInterface.VertexLayoutId.has_value())
            {
                const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                    manager.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
                if (!layoutResult)
                    return std::unexpected(layoutResult.error().Detail);

                vertexBufferLayout = layoutResult->Get()->GetLayout();
            }

            return Renderer::GraphicsPipeline::Create(context, {
                .Name = fmt::format("Material {} Pipeline", id.Value),
                .ColorAttachments = {
                    {.Format = Renderer::GBuffer::AlbedoFormat, .Blend = Renderer::BlendState::Opaque()},
                    {.Format = Renderer::GBuffer::NormalFormat, .Blend = Renderer::BlendState::Opaque()},
                    {.Format = Renderer::GBuffer::ORMFormat, .Blend = Renderer::BlendState::Opaque()},
                },
                .DepthAttachmentFormat = Renderer::GBuffer::DepthFormat,
                .VertexBufferLayout = vertexBufferLayout,
                .PipelineLayout = layout,
                .ShaderStages = {
                    {.Stage = Renderer::ShaderStage::Vertex,   .Module = vsAsset.Module},
                    {.Stage = Renderer::ShaderStage::Fragment, .Module = fsAsset.Module},
                },
                .CullMode = Renderer::CullMode::Back,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
            });
        }
    }

    AssetResult<Detail::LoadJob> MaterialLoader::Load(
        AssetManager& manager, Renderer::Context& context, TaskSystem& /*tasks*/,
        TypeRegistry& /*types*/, AssetId id, std::span<const u8> cooked, bool async) const
    {
        // ── 1. CookedMaterialHeader ──────────────────────────────────────────
        if (cooked.size() < sizeof(CookedMaterialHeader))
            return std::unexpected(Corrupt(id, "material: cooked blob smaller than CookedMaterialHeader"));

        CookedMaterialHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        usize cursor = sizeof(CookedMaterialHeader);

        // A stale blob is a recoverable Corrupt error, not a silent reinterpretation or a crash.
        if (header.Version != CookedMaterialVersion)
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "material: blob version {} does not match CookedMaterialVersion {} — "
                "the material format has changed; re-cook the pack",
                header.Version, CookedMaterialVersion)));
        }

        // Domain is stored as the underlying integer; the cook validates the fragment
        // outputs against the domain's contract, so the runtime asserts range and trusts it.
        VE_ASSERT(header.Domain <= static_cast<u32>(MaterialDomain::PostProcess),
            "material: header Domain {} is out of range for MaterialDomain", header.Domain);
        const MaterialDomain domain = static_cast<MaterialDomain>(header.Domain);

        // The single block must fit the registry's per-material param stride.
        if (header.BlockBytes > Renderer::BindlessRegistry::MaterialParamStride)
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "material: BlockBytes {} exceeds MaterialParamStride {}",
                header.BlockBytes, Renderer::BindlessRegistry::MaterialParamStride)));
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

        // ── 3. The single param block ────────────────────────────────────────
        if (cooked.size() < cursor + header.BlockBytes)
            return std::unexpected(Corrupt(id, "material: cooked blob smaller than param block"));

        vector<std::byte> block(header.BlockBytes);
        if (header.BlockBytes > 0)
            std::memcpy(block.data(), cooked.data() + cursor, header.BlockBytes);
        cursor += header.BlockBytes;

        // ── 4. Fan out shader sub-loads ──────────────────────────────────────
        // Finalize runs only once both shaders are resident (dependencies finalize before the parent).
        vector<Ref<Detail::AssetCacheEntry>> dependencies;

        auto loadShader = [&](u64 shaderId) -> AssetResult<AssetHandle<Veng::Shader>>
        {
            if (async)
            {
                AssetHandle<Veng::Shader> handle = manager.Load<Veng::Shader>(AssetId{shaderId});
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency, .Id = AssetId{shaderId},
                        .Detail = fmt::format("material {}: shader dependency {} did not resolve",
                                              id.Value, shaderId)});
                }
                return handle;
            }
            return manager.LoadSync<Veng::Shader>(AssetId{shaderId});
        };

        const AssetResult<AssetHandle<Veng::Shader>> vsResult = loadShader(header.VertexShaderId);
        if (!vsResult)
            return std::unexpected(vsResult.error());

        const AssetResult<AssetHandle<Veng::Shader>> fsResult = loadShader(header.FragmentShaderId);
        if (!fsResult)
            return std::unexpected(fsResult.error());

        const AssetHandle<Veng::Shader> vsHandle = *vsResult;
        const AssetHandle<Veng::Shader> fsHandle = *fsResult;
        dependencies.push_back(AssetManager::EntryOf(vsHandle));
        dependencies.push_back(AssetManager::EntryOf(fsHandle));

        // ── 5. Build MaterialField table + fan out texture sub-loads ─────────
        vector<Veng::MaterialField> fields;
        fields.reserve(header.FieldCount);

        // Track textures by AssetId to deduplicate.
        vector<u64> textureIds;
        vector<AssetHandle<Veng::Texture>> textures;

        for (u32 i = 0; i < header.FieldCount; ++i)
        {
            const CookedMaterialField& cf = cookedFields[i];

            Veng::MaterialField::FieldKind kind;
            switch (cf.Kind)
            {
                case 0: kind = Veng::MaterialField::FieldKind::Param;          break;
                case 1: kind = Veng::MaterialField::FieldKind::TextureHandle;  break;
                case 2: kind = Veng::MaterialField::FieldKind::SamplerHandle;  break;
                default:
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' has unrecognized Kind {}",
                        i, BridgeName(cf.Name), cf.Kind)));
            }

            const bool isHandle = kind == Veng::MaterialField::FieldKind::TextureHandle
                               || kind == Veng::MaterialField::FieldKind::SamplerHandle;

            if (isHandle)
            {
                if (cf.Offset + sizeof(u32) > header.BlockBytes)
                {
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' offset {} + 4 exceeds block size {}",
                        i, BridgeName(cf.Name), cf.Offset, header.BlockBytes)));
                }

                // A handle field with no cooked asset (TextureId == 0) is
                // runtime-bound — the renderer writes its bindless index per frame
                // via Material::SetTextureHandle/SetSamplerHandle. It loads no
                // texture dependency and its slot stays zero until then.
                if (cf.TextureId == 0)
                {
                    fields.push_back(Veng::MaterialField{
                        .Name      = BridgeName(cf.Name),
                        .Offset    = cf.Offset,
                        .Size      = cf.Size,
                        .Kind      = kind,
                        .TextureId = 0,
                    });
                    continue;
                }

                // Load (or reuse) the texture asset. The resolved bindless index
                // is patched into params by Material::Finalize, once the texture
                // is registered — not here.
                bool known = false;
                for (const u64 existing : textureIds)
                {
                    if (existing == cf.TextureId) { known = true; break; }
                }

                if (!known)
                {
                    AssetHandle<Veng::Texture> texHandle;
                    if (async)
                    {
                        texHandle = manager.Load<Veng::Texture>(AssetId{cf.TextureId});
                        if (!AssetManager::EntryOf(texHandle))
                        {
                            return std::unexpected(AssetLoadError{
                                .Kind = AssetError::MissingDependency, .Id = AssetId{cf.TextureId},
                                .Detail = fmt::format("material {}: texture dependency {} did not resolve",
                                                      id.Value, cf.TextureId)});
                        }
                    }
                    else
                    {
                        const AssetResult<AssetHandle<Veng::Texture>> texResult =
                            manager.LoadSync<Veng::Texture>(AssetId{cf.TextureId});
                        if (!texResult)
                            return std::unexpected(texResult.error());
                        texHandle = *texResult;
                    }

                    textureIds.push_back(cf.TextureId);
                    textures.push_back(texHandle);
                    dependencies.push_back(AssetManager::EntryOf(texHandle));
                }
            }

            fields.push_back(Veng::MaterialField{
                .Name      = BridgeName(cf.Name),
                .Offset    = cf.Offset,
                .Size      = cf.Size,
                .Kind      = kind,
                .TextureId = isHandle ? cf.TextureId : 0,
            });
        }

        // ── 6. Construct the unregistered Material ───────────────────────────
        const Veng::MaterialInfo info{
            .Name           = fmt::format("Material {}", id.Value),
            .Context        = &context,
            .Domain         = domain,
            .Pipeline       = nullptr,
            .VertexShader   = vsHandle,
            .FragmentShader = fsHandle,
            .Textures       = std::move(textures),
            .Block          = std::move(block),
            .Fields         = std::move(fields),
            .SelectorOffset = SelectorPushOffsetFor(domain),
        };

        const Ref<Veng::Material> material = Veng::Material::Create(info);

        // ── 7. The main-thread finalize ──────────────────────────────────────
        return Detail::LoadJob{
            .Resource = Detail::RefAny(material),
            .Dependencies = std::move(dependencies),
            .Finalize = [&manager, &context, id, domain, vsHandle, fsHandle, material]() -> VoidResult
            {
                Result<Ref<Renderer::PipelineLayout>> layout =
                    BuildPipelineLayout(context, id, domain, *vsHandle.Get(), *fsHandle.Get());
                if (!layout)
                    return std::unexpected(layout.error());

                Ref<Renderer::GraphicsPipeline> pipeline;
                if (domain == MaterialDomain::Surface)
                {
                    Result<Ref<Renderer::GraphicsPipeline>> built =
                        BuildSurfacePipeline(manager, context, id, *layout, *vsHandle.Get(), *fsHandle.Get());
                    if (!built)
                        return std::unexpected(built.error());
                    pipeline = std::move(*built);
                }

                material->Finalize(std::move(*layout), std::move(pipeline));
                return {};
            },
        };
    }
}
