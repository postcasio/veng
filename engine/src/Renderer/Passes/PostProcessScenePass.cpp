#include <Veng/Renderer/ScenePass.h>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    PostProcessScenePass::PostProcessScenePass(Context& context,
                                               AssetHandle<MaterialInstance> material,
                                               PostProcessInput input, ResourceId output,
                                               Format outputFormat, uvec2 extent)
        : m_Context(context), m_Material(std::move(material)), m_Input(std::move(input)),
          m_Output(output), m_OutputFormat(outputFormat), m_Extent(extent)
    {
    }

    void PostProcessScenePass::BuildPipeline()
    {
        VE_ASSERT(m_Material.IsLoaded(),
                  "PostProcessScenePass: the PostProcess material is not resident");

        const MaterialInstance& material = *m_Material.Get();
        VE_ASSERT(material.GetDomain() == MaterialDomain::PostProcess,
                  "PostProcessScenePass: material '{}' is not a PostProcess material",
                  material.GetName());

        // The layout (set 0 reserved, selector push range) comes from the material loader;
        // only this color-format-dependent GraphicsPipeline is the pass's to create.
        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("PostProcess Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_OutputFormat}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
    }

    void PostProcessScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        if (!m_Pipeline)
        {
            BuildPipeline();
        }

        const PostProcessInput input = m_Input;
        const PostProcessExtraInput extra = m_Extra;
        const bool hasExtra = extra.Texture.IsValid();

        // A two-source pass (bloom composite) declares .Sample on both ids for the
        // graph-derived barriers; single-source passes declare only the primary input.
        RenderGraph::PassBuilder builder = graph.AddPass("PostProcess");
        builder
            .Color({
                .Resource = m_Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(input.Source);
        if (hasExtra)
        {
            builder.Sample(extra.Source);
        }

        builder.Execute(
            [this, input, extra, hasExtra](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                MaterialInstance& material = *m_Material.Get();

                // Write the live upstream bindless slots; must precede Material::Bind
                // so the pushed selector reads this frame's region.
                material.SetTextureHandle(input.TextureField, input.SourceTexture);
                material.SetSamplerHandle(input.SamplerField, input.Sampler);
                if (hasExtra)
                {
                    material.SetTextureHandle(extra.TextureField, extra.Texture);
                    material.SetSamplerHandle(extra.SamplerField, extra.Sampler);
                }

                // Pipeline must be bound before registry.Bind (Bind uses the active layout).
                // The terminal post pass renders the FULL output and upscales its sub-rect source
                // (the tonemap material samples through ScaledSampleUV), so the output image is
                // valid edge to edge and the compositor needs no sub-rect awareness.
                cmd.BindPipeline(m_Pipeline);
                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);
                m_Context.GetBindlessRegistry().Bind(cmd);
                material.Bind(cmd);
                cmd.DrawFullscreenTriangle();
            });
    }
}
