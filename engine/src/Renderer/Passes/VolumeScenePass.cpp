#include "VolumeScenePass.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/VolumeField.h>
#include <Veng/Scene/Camera.h>

namespace Veng::Renderer
{
    namespace
    {
        // The volume-march fragment shader in the engine core pack (auto-mounted by AssetManager).
        constexpr AssetId VolumeFieldFragId{0xE92C90AE36AC3607ULL};

        // The fullscreen vertex stage shared by every fullscreen pass (the engine core pack).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};

        // Matches volume_field.frag PushConstants byte-for-byte.
        struct VolumeFieldPush
        {
            vec4 BoundsMin;   // xyz world-space AABB min
            vec4 BoundsMax;   // xyz world-space AABB max
            vec4 Scales;      // x emission factor, y extinction factor (both Opacity-folded)
            u32 DepthTexture; // g-buffer depth bindless index
            u32 Sampler;      // shared sampler bindless index
            u32 ViewConstantsIndex;
            u32 Steps;
            u32 FrameIndex;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, AssetId id, const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "VolumeScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }
    }

    VolumeScenePass::VolumeScenePass(Context& context, AssetManager& assets,
                                     const vector<VolumeFieldInstance>* fields,
                                     const Format outputFormat, const SamplerHandle samplerHandle)
        : m_Context(context), m_Fields(fields), m_OutputFormat(outputFormat),
          m_SamplerHandle(samplerHandle)
    {
        const AssetHandle<Veng::Shader> vs =
            LoadShader(assets, FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> fs =
            LoadShader(assets, VolumeFieldFragId, "volume march fragment");

        // Set 1 binding 0: the field's 3D sampled image; binding 1: its sampler — a closed,
        // per-field dedicated set off bindless (a Type3D descriptor in set 0's Metal argument buffer
        // is the MoltenVK risk the engine refuses, the IBL-cubemap precedent).
        m_SetLayout = DescriptorSetLayout::Create(m_Context,
                                                  {
                                                      .Name = "VolumeField Set Layout",
                                                      .Bindings =
                                                          {
                                                              {.Binding = 0,
                                                               .Type = DescriptorType::SampledImage,
                                                               .Count = 1,
                                                               .Stages = ShaderStage::Fragment},
                                                              {.Binding = 1,
                                                               .Type = DescriptorType::Sampler,
                                                               .Count = 1,
                                                               .Stages = ShaderStage::Fragment},
                                                          },
                                                  });
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "VolumeField Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<VolumeFieldPush>(
                               ShaderStage::Fragment)},
                       });

        // (srcColor = ONE, dstColor = SRC_ALPHA): finalRgb = emission + background·transmittance.
        // Alpha stays untouched (the lit HDR target's alpha is unused downstream).
        const BlendState marchBlend{
            .Enable = true,
            .SrcColorFactor = BlendFactor::One,
            .DstColorFactor = BlendFactor::SrcAlpha,
            .ColorOp = BlendOp::Add,
            .SrcAlphaFactor = BlendFactor::Zero,
            .DstAlphaFactor = BlendFactor::One,
            .AlphaOp = BlendOp::Add,
        };
        m_Pipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "VolumeField March Pipeline",
                           .ColorAttachments = {{.Format = m_OutputFormat, .Blend = marchBlend}},
                           .PipelineLayout = m_Layout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                               },
                       });
    }

    VolumeScenePass::~VolumeScenePass() = default;

    const Ref<DescriptorSet>& VolumeScenePass::SetFor(const VolumeField* const field)
    {
        Ref<DescriptorSet>& set = m_FieldSets[field];
        if (set == nullptr)
        {
            // The 3D view + sampler are immutable for the field's life, so the set is written once
            // and bound every frame without a per-frame rewrite (no frames-in-flight ring needed).
            set = DescriptorSet::Create(m_Context,
                                        {.Name = "VolumeField Set", .Layout = m_SetLayout});
            set->Write(0, field->GetImageView());
            set->Write(1, field->GetSampler());
        }
        return set;
    }

    void VolumeScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        m_DepthTextureIndex = io.DepthHandle.Index;
        m_SamplerIndex = m_SamplerHandle.Index;

        graph
            .AddPass("Volume Fields")
            // Composite into the lit scene color (preserve it; the blend adds glow and attenuates).
            .Color({
                .Resource = io.Hdr,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(io.GBufferDepth)
            .Execute(
                [this](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    DrawFields(ctx.Cmd(), ctx);
                });
    }

    void VolumeScenePass::DrawFields(CommandBuffer& cmd, const ScenePassContext& ctx)
    {
        // A monotonic counter feeds the shader's temporal jitter rotation (independent of the
        // frames-in-flight cycle, so the per-pixel start offset decorrelates every real frame).
        const u32 frameIndex = m_FrameIndex++;

        if (m_Fields == nullptr || m_Fields->empty())
        {
            m_FieldSets.clear();
            return;
        }

        const SceneView& view = ctx.View();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const uvec2 renderExtent = view.RenderExtent;
        const vec3 cameraPos = view.Camera.GetPosition();

        // Draw far-to-near by camera distance to bounds center, so each nearer field's blend
        // attenuates what the farther ones already composited (a documented per-field approximation).
        std::vector<const VolumeFieldInstance*> ordered;
        ordered.reserve(m_Fields->size());
        for (const VolumeFieldInstance& inst : *m_Fields)
        {
            if (inst.Field != nullptr)
            {
                ordered.push_back(&inst);
            }
        }
        std::ranges::sort(ordered,
                          [&](const VolumeFieldInstance* a, const VolumeFieldInstance* b)
                          {
                              return VolumeFieldFartherFirst(cameraPos, a->Field->GetBounds(),
                                                             b->Field->GetBounds());
                          });

        cmd.BindPipeline(m_Pipeline);
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);
        registry.Bind(cmd);

        for (const VolumeFieldInstance* inst : ordered)
        {
            const VolumeField* field = inst->Field;
            const AABB& bounds = field->GetBounds();

            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {SetFor(field)},
                .FirstSet = 3, // sets 0-2 are the bindless registries, bound once above
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(VolumeFieldPush{
                .BoundsMin = vec4(bounds.Min, 0.0f),
                .BoundsMax = vec4(bounds.Max, 0.0f),
                .Scales = vec4(inst->EmissionScale * inst->Opacity,
                               inst->ExtinctionScale * inst->Opacity, 0.0f, 0.0f),
                .DepthTexture = m_DepthTextureIndex,
                .Sampler = m_SamplerIndex,
                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                .Steps = inst->Steps,
                .FrameIndex = frameIndex,
            });
            cmd.DrawFullscreenTriangle();
        }

        // Prune sets for fields no longer resolved (freeing their descriptors).
        std::unordered_set<const VolumeField*> live;
        live.reserve(ordered.size());
        for (const VolumeFieldInstance* inst : ordered)
        {
            live.insert(inst->Field);
        }
        for (auto it = m_FieldSets.begin(); it != m_FieldSets.end();)
        {
            if (live.contains(it->first))
            {
                ++it;
            }
            else
            {
                it = m_FieldSets.erase(it);
            }
        }
    }
}
