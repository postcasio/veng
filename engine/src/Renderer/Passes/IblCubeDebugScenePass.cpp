#include "IblCubeDebugScenePass.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneRenderer.h>

namespace Veng::Renderer
{
    namespace
    {
        // Matches ibl_cube_blit.frag PushConstants byte-for-byte (16 bytes).
        struct IblCubePushConstants
        {
            u32 ViewConstantsIndex;
            f32 Lod;
            f32 Intensity;
            u32 Enabled;
        };
    }

    IblCubeDebugScenePass::IblCubeDebugScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                                 Ref<DescriptorSetLayout> setLayout, uvec2 extent)
        : m_Context(context), m_Pipeline(std::move(pipeline)), m_SetLayout(std::move(setLayout)),
          m_Extent(extent)
    {
    }

    void IblCubeDebugScenePass::SetCube(const Ref<ImageView>& cube, const Ref<Sampler>& sampler,
                                        const f32 lod, const bool enabled)
    {
        m_Lod = lod;
        m_Enabled = enabled;

        // Rebuild the set only when the cube or sampler moves — a stable source records no
        // descriptor writes, and the retire path makes replacing the old set mid-run safe. The cube
        // is always bound (the shader statically uses set 3); enabled draws it or black.
        if (cube.get() == m_SetCube && sampler.get() == m_SetSampler)
        {
            return;
        }
        m_Set =
            DescriptorSet::Create(m_Context, {.Name = "IBL Cube Debug Set", .Layout = m_SetLayout});
        m_Set->Write(0, cube);
        m_Set->Write(1, sampler);
        m_SetCube = cube.get();
        m_SetSampler = sampler.get();
    }

    void IblCubeDebugScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId output = io.Output;

        graph.AddPass("IBL Cube Debug")
            .Color({
                .Resource = output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Execute(
                [this](PassContext& inner)
                {
                    // No cube fed yet (a recompile frame before ResolveScenePasses reaches the feed,
                    // or a frame it early-returned): the attachment is already cleared to black by
                    // the LoadOp, so draw nothing — binding a null set would fault, and skipping the
                    // draw leaves set 3 unused so the static-use rule is satisfied too.
                    if (m_Set == nullptr)
                    {
                        return;
                    }

                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const SceneView& view = ctx.View();

                    const uvec2 renderExtent = view.RenderExtent;
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);

                    // The cube set is always bound (the shader statically uses set 3); Enabled 0
                    // draws black without sampling it.
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_Set},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });

                    cmd.PushConstants(IblCubePushConstants{
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Lod = m_Lod,
                        .Intensity = view.Exposure,
                        .Enabled = m_Enabled ? 1u : 0u,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
