#include "PostProcessEffectScenePass.h"

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    PostProcessEffectScenePass::PostProcessEffectScenePass(
        Context& context, AssetManager& assets, Format outputFormat, uvec2 extent,
        uvec2 renderExtent, Ref<GraphicsPipeline> passthroughPipeline)
        : m_Context(context), m_Assets(assets), m_OutputFormat(outputFormat), m_Extent(extent),
          m_RenderExtent(renderExtent), m_PassthroughPipeline(std::move(passthroughPipeline))
    {
    }

    void PostProcessEffectScenePass::SetMaterial(AssetHandle<MaterialInstance> material)
    {
        m_Material = std::move(material);
    }

    void PostProcessEffectScenePass::SetWiring(ResourceId source, TextureHandle sourceHandle,
                                               ResourceId output)
    {
        m_Source = source;
        m_SourceHandle = sourceHandle;
        m_Output = output;
    }

    void PostProcessEffectScenePass::BuildPipeline()
    {
        VE_ASSERT(m_Material.IsLoaded(),
                  "PostProcessEffectScenePass: the effect material is not resident");

        const MaterialInstance& material = *m_Material.Get();
        VE_ASSERT(material.GetDomain() == MaterialDomain::PostProcess,
                  "PostProcessEffectScenePass: material '{}' is not a PostProcess material",
                  material.GetName());

        // Only this color-format-dependent pipeline is the pass's to build; the layout (set 0
        // reserved, the post selector push range) comes from the material loader.
        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("PostProcess Effect Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_OutputFormat}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
        m_PipelineMaterialId = m_Material.Id().Value;

        // The per-frame inputs are addressed by handle from here on: the names are the pass's own
        // and the schema is the material's, so both are settled the moment the material is.
        m_Fields = EffectFields{
            .Scene = material.Field("Scene"),
            .SceneSampler = material.Field("SceneSampler"),
            .Depth = material.Field("Depth"),
            .DepthSampler = material.Field("DepthSampler"),
            .SceneScaleUv = material.Field("SceneScaleUV"),
            .DepthScaleUv = material.Field("DepthScaleUV"),
        };
    }

    void PostProcessEffectScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        // Contributed while the effect is in the active set; the material is bound per frame and may
        // arrive after the graph compiled, so the draw is gated in Execute, never the pass. The
        // output is a fresh ping-pong target, so it clears rather than loads — but when no material
        // is bound (a Rebuild after the frame's material forward reconstructed this pass), the Execute
        // copies the source through rather than leaving that black clear as the scene color.
        const ResourceId source = m_Source;
        const ResourceId depthId = io.GBufferDepth;
        const TextureHandle depthHandle = io.DepthHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;

        graph.AddPass("PostProcess Effect")
            .Color({
                .Resource = m_Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(source)
            .Sample(depthId)
            .Execute(
                [this, depthHandle, samplerHandle](PassContext& inner)
                {
                    if (!m_Material.IsLoaded())
                    {
                        // Copy the source through unchanged: the effect target's LoadOp::Clear left
                        // it black, and it is this frame's scene-color source for the tail, so
                        // returning here would blank the scene for the frame. The source fills the
                        // post-resolve allocation, so a full-UV copy is the identity mapping.
                        const ScenePassContext ctx = Wrap(inner);
                        CommandBuffer& cmd = ctx.Cmd();
                        const SceneView& view = ctx.View();
                        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                        cmd.BindPipeline(m_PassthroughPipeline);
                        cmd.SetViewport({0, 0}, view.PostResolveExtent);
                        cmd.SetScissor({0, 0}, view.PostResolveExtent);
                        registry.Bind(cmd);
                        cmd.PushConstants(PostProcessPassthroughPush{
                            .SourceTexture = m_SourceHandle.Index,
                            .Sampler = samplerHandle.Index,
                        });
                        cmd.DrawFullscreenTriangle();
                        return;
                    }
                    // Build the output-format pipeline on first use and when the bound material
                    // identity changes (the material may not have been resident when the graph
                    // compiled).
                    if (!m_Pipeline || m_PipelineMaterialId != m_Material.Id().Value)
                    {
                        BuildPipeline();
                    }

                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    // The scale maps below are derived from the recording view, and a material's
                    // block rings by frame-in-flight rather than by view, so writing them into the
                    // resolved instance would hand every viewport sharing it the last one recorded.
                    // This renderer's own mirror is what the writes and the draw go through.
                    MaterialInstance& resolved = *m_Material.Get();
                    MaterialInstance& material =
                        m_ViewMaterial.Resolve(m_Assets, m_Context, resolved,
                                               fmt::format("{} (View)", resolved.GetName()));

                    // The finished scene color fills the post-resolve allocation the promotion
                    // handed on; the g-buffer depth is the rendered sub-rect of the render
                    // allocation. Both maps are the identity at render scale 1.0.
                    const vec2 alloc = vec2(m_Extent);
                    const vec2 renderAlloc = vec2(m_RenderExtent);
                    const vec2 post = vec2(view.PostResolveExtent);
                    const vec2 render = vec2(view.RenderExtent);
                    const vec4 sceneScaleUv = vec4(post / alloc, (post - 0.5f) / alloc);
                    const vec4 depthScaleUv =
                        vec4(render / renderAlloc, (render - 0.5f) / renderAlloc);

                    // Write the runtime-bound inputs into the material's fields where it declares
                    // them, so an effect that ignores depth (or the scale maps) need not declare
                    // those fields. The handles were resolved with the pipeline.
                    if (m_Fields.Scene.IsValid())
                    {
                        material.SetTextureHandle(m_Fields.Scene, m_SourceHandle);
                    }
                    if (m_Fields.SceneSampler.IsValid())
                    {
                        material.SetSamplerHandle(m_Fields.SceneSampler, samplerHandle);
                    }
                    if (m_Fields.Depth.IsValid())
                    {
                        material.SetTextureHandle(m_Fields.Depth, depthHandle);
                    }
                    if (m_Fields.DepthSampler.IsValid())
                    {
                        material.SetSamplerHandle(m_Fields.DepthSampler, samplerHandle);
                    }
                    if (m_Fields.SceneScaleUv.IsValid())
                    {
                        material.SetParam(m_Fields.SceneScaleUv, sceneScaleUv);
                    }
                    if (m_Fields.DepthScaleUv.IsValid())
                    {
                        material.SetParam(m_Fields.DepthScaleUv, depthScaleUv);
                    }

                    // The effect writes the post-resolve region (what bloom and the tonemap treat as
                    // the scene-color region), so its output is a drop-in scene-color source.
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, view.PostResolveExtent);
                    cmd.SetScissor({0, 0}, view.PostResolveExtent);
                    registry.Bind(cmd);
                    // A PostProcess instance's Bind pushes only its selector at the post domain's
                    // offset; the field writes above landed in this frame's ring region first.
                    material.Bind(cmd);
                    cmd.DrawFullscreenTriangle();
                });
    }
}
