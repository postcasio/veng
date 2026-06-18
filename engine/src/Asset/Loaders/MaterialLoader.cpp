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

    namespace
    {
        // Build the material's forward graphics pipeline from the (now-resident)
        // vertex/fragment shader interfaces. Runs on the main thread at finalize:
        // the shaders are guaranteed resident, so their Interface is readable, and
        // the GPU pipeline build is main-thread-only work. A drifted shader (no
        // selector push-constant range) is a recoverable Corrupt failure.
        Result<Ref<Renderer::GraphicsPipeline>> BuildPipeline(
            AssetManager& manager, Renderer::Context& context, AssetId id,
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
                return std::unexpected(fmt::format(
                    "material: no merged push-constant range covers [{}+4) — "
                    "the shader does not declare the expected forward materialIndex selector",
                    MaterialSelectorPushOffset));
            }

            const Ref<Renderer::PipelineLayout> pipelineLayout = Renderer::PipelineLayout::Create(context, {
                .Name                = fmt::format("Material {} Layout", id.Value),
                .DescriptorSetLayouts = descLayouts,
                .PushConstantRanges  = mergedRanges,
            });

            // Vertex layout: resolve from the vertex shader's declared
            // VertexLayoutId. CPU-only and instant, so a synchronous load here is
            // free even on the async path.
            optional<Renderer::VertexBufferLayout> vertexBufferLayout;
            if (vsInterface.VertexLayoutId.has_value())
            {
                const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                    manager.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
                if (!layoutResult)
                    return std::unexpected(layoutResult.error().Detail);

                vertexBufferLayout = layoutResult->Get()->GetLayout();
            }

            // An opaque material renders into the deferred g-buffer: two color
            // attachments (G0 albedo, G1 world-normal) and the shared depth
            // attachment, each color target opaque (no blend). The fragment
            // shader writes both through GBufferOutput; the attachment formats are
            // the fixed g-buffer contract, not the context's output format.
            return Renderer::GraphicsPipeline::Create(context, {
                .Name = fmt::format("Material {} Pipeline", id.Value),
                .ColorAttachments = {
                    {.Format = Renderer::GBuffer::AlbedoFormat, .Blend = Renderer::BlendState::Opaque()},
                    {.Format = Renderer::GBuffer::NormalFormat, .Blend = Renderer::BlendState::Opaque()},
                },
                .DepthAttachmentFormat = Renderer::GBuffer::DepthFormat,
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

        // The format guard: a stale blob is a loud reject, not a silent
        // reinterpretation. A Corrupt error (not a VE_ASSERT) gives the user a
        // clear load failure rather than a crash.
        if (header.Version != CookedMaterialVersion)
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "material: blob version {} does not match CookedMaterialVersion {} — "
                "the material format has changed; re-cook the pack",
                header.Version, CookedMaterialVersion)));
        }

        // The domain is stored as the underlying integer; cast guarded by a loud
        // assert on an out-of-range value (the one-line-fix-on-drift pattern the
        // other underlying-int enum fields use). The cook validates the fragment
        // outputs against the domain's contract, so the runtime trusts it.
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
        // Async fans these out as concurrent async loads; sync blocks on each.
        // Either way the material's Finalize runs only once both are resident
        // (the manager orders the dependencies' finalizes before the parent's).
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
                if (cf.TextureId == 0)
                {
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' is a handle field but TextureId is 0",
                        i, BridgeName(cf.Name))));
                }

                if (cf.Offset + sizeof(u32) > header.BlockBytes)
                {
                    return std::unexpected(Corrupt(id, fmt::format(
                        "material: field {} '{}' offset {} + 4 exceeds block size {}",
                        i, BridgeName(cf.Name), cf.Offset, header.BlockBytes)));
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
            .SelectorOffset = MaterialSelectorPushOffset,
        };

        const Ref<Veng::Material> material = Veng::Material::Create(info);

        // ── 7. The main-thread finalize ──────────────────────────────────────
        // Runs only once every dependency (shaders + textures) is resident: build
        // the pipeline from their interfaces, then register + patch texture
        // indices via Material::Finalize.
        return Detail::LoadJob{
            .Resource = Detail::RefAny(material),
            .Dependencies = std::move(dependencies),
            .Finalize = [&manager, &context, id, vsHandle, fsHandle, material]() -> VoidResult
            {
                Result<Ref<Renderer::GraphicsPipeline>> pipeline =
                    BuildPipeline(manager, context, id, *vsHandle.Get(), *fsHandle.Get());
                if (!pipeline)
                    return std::unexpected(pipeline.error());

                material->Finalize(std::move(*pipeline));
                return {};
            },
        };
    }
}
