#include "ParticleScenePass.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "../ParticleSimulation.h"

namespace Veng::Renderer
{
    namespace
    {
        // The draw's shaders in the engine core pack. The masked fragment writes the bloom mask as
        // a second output, a reactive one the reactive mask after any bloom mask; the plain one
        // writes colour alone.
        constexpr AssetId ParticleVertId{0x20AF46B44DED5CA8ULL};
        constexpr AssetId ParticleFragId{0x63F035907DD5EEA6ULL};
        constexpr AssetId ParticleMaskedFragId{0xC58EA21E7F99F47BULL};
        constexpr AssetId ParticleReactiveFragId{0x8DF816C7AB0BCB67ULL};
        constexpr AssetId ParticleMaskedReactiveFragId{0x73156E81A78E4012ULL};

        // Matches particle_draw.slang's ParticleDrawPush.
        struct ParticleDrawPush
        {
            u32 ViewConstantsIndex = 0;
            u32 List = 0;
            u32 Pad0 = 0;
            u32 Pad1 = 0;
        };

        // Which list a draw's vertices index.
        constexpr u32 AlphaList = 0;
        constexpr u32 AdditiveList = 1;

        // Additive over the lit colour, leaving the target's alpha alone, as the sprite pass's.
        BlendState ParticleAdditiveBlend()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::One,
                .DstColorFactor = BlendFactor::One,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::Zero,
                .DstAlphaFactor = BlendFactor::One,
                .AlphaOp = BlendOp::Add,
            };
        }

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, const AssetId id,
                                             const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "ParticleScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }
    }

    ParticleScenePass::ParticleScenePass(Context& context, AssetManager& assets,
                                         const ParticleScenePassInfo& info)
        : m_Context(context), m_Simulation(info.Simulation), m_TargetId(info.Target),
          m_DepthId(info.Depth), m_MaskId(info.Mask), m_ReactiveId(info.Reactive),
          m_SceneDepthCopyId(info.SceneDepthCopy), m_Forward(info.Forward)
    {
        VE_ASSERT(m_Simulation != nullptr, "ParticleScenePass: no particle battery");
        const AssetHandle<Veng::Shader> vs = LoadShader(assets, ParticleVertId, "particle vertex");
        const AssetId fragId =
            m_MaskId.IsValid()
                ? (m_ReactiveId.IsValid() ? ParticleMaskedReactiveFragId : ParticleMaskedFragId)
                : (m_ReactiveId.IsValid() ? ParticleReactiveFragId : ParticleFragId);
        const AssetHandle<Veng::Shader> fs = LoadShader(assets, fragId, "particle fragment");

        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "Particle Draw Layout",
                           .DescriptorSetLayouts = {m_Simulation->GetSetLayout()},
                           .PushConstantRanges = {PushConstantRange::Of<ParticleDrawPush>(
                               ShaderStage::Vertex | ShaderStage::Fragment)},
                       });

        const auto makePipeline = [&](const char* name, const BlendState& colorBlend)
        {
            vector<PipelineAttachmentInfo> attachments = {
                {.Format = info.TargetFormat, .Blend = colorBlend}};
            if (m_MaskId.IsValid())
            {
                attachments.push_back({.Format = info.MaskFormat, .Blend = BlendState::Additive()});
            }
            if (m_ReactiveId.IsValid())
            {
                attachments.push_back(
                    {.Format = info.ReactiveFormat, .Blend = BlendState::Additive()});
            }
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = std::move(attachments),
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .PipelineLayout = m_Layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .Topology = PrimitiveTopology::TriangleList,
                               // A rolled quad may present either winding; a particle has no back.
                               .CullMode = CullMode::None,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = false,
                               // Reverse-Z: a nearer fragment has larger depth.
                               .DepthCompareOp = CompareOp::GreaterOrEqual,
                           });
        };
        m_AlphaPipeline = makePipeline("Particle Alpha Pipeline", BlendState::PremultipliedAlpha());
        m_AdditivePipeline = makePipeline("Particle Additive Pipeline", ParticleAdditiveBlend());
    }

    ParticleScenePass::~ParticleScenePass() = default;

    void ParticleScenePass::DeclareAhead(RenderGraph& graph, const PassIO& io)
    {
        m_Simulation->Declare(graph, io, m_Forward);
    }

    void ParticleScenePass::DeclareInputs(RenderGraph::PassBuilder& builder,
                                          const PassIO& /*io*/) const
    {
        if (m_SceneDepthCopyId.IsValid())
        {
            builder.Sample(m_SceneDepthCopyId);
        }
        builder.StorageBufferRead(m_Simulation->GetDrawsId())
            .StorageBufferRead(m_Simulation->GetAlphaKeysId())
            .StorageBufferRead(m_Simulation->GetAdditiveId())
            .IndirectRead(m_Simulation->GetArgsId());
    }

    bool ParticleScenePass::IsDrawing() const
    {
        return m_Simulation->IsDrawing();
    }

    void ParticleScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        DeclareAhead(graph, io);

        RenderGraph::PassBuilder builder = graph.AddPass("Scene Particles");
        builder.Color({.Resource = m_TargetId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        if (m_MaskId.IsValid())
        {
            // The translucent pass cleared the mask; particles add to it.
            builder.Color({.Resource = m_MaskId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        if (m_ReactiveId.IsValid())
        {
            // Cleared ahead of the scene's composited layers; particles add their coverage.
            builder.Color(
                {.Resource = m_ReactiveId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        builder.Depth({.Resource = m_DepthId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        DeclareInputs(builder, io);
        builder.SkipWhen([this] { return !IsDrawing(); });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    void ParticleScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const uvec2 extent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, extent);
        cmd.SetScissor({0, 0}, extent);

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 frame = m_Context.GetCurrentFrameInFlight();
        const auto draw =
            [&](const Ref<GraphicsPipeline>& pipeline, const u32 list, const u64 offset)
        {
            cmd.BindPipeline(pipeline);
            registry.Bind(cmd);
            cmd.BindDescriptorSets({m_Simulation->GetSet(frame).get()},
                                   BindlessRegistry::FirstUserSet, PipelineBindPoint::Graphics);
            cmd.PushConstants(ParticleDrawPush{
                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(), .List = list});
            cmd.DrawIndirect(m_Simulation->GetArgs(), offset, 1, 16);
        };
        draw(m_AlphaPipeline, AlphaList, ParticleSimulation::AlphaDrawOffset);
        draw(m_AdditivePipeline, AdditiveList, ParticleSimulation::AdditiveDrawOffset);
    }
}
