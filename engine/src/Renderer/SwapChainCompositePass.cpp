#include <Veng/Renderer/SwapChainCompositePass.h>

#include <Veng/Assert.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>

#include <Veng/ImGui/ImGuiLayer.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    namespace
    {
        // Core pack fullscreen vertex stage, shared with SceneRenderer's blits.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId CompositeFragId{0x4BCFF3ED0254B42EULL};

        // Bindless scene/ImGui texture and sampler indices for the composite shader.
        // Matches composite.frag PushConstants byte-for-byte.
        struct CompositePushConstants
        {
            u32 SceneTexture;
            u32 ImGuiTexture;
            u32 Sampler;
        };
    }

    struct SwapChainCompositePass::Impl
    {
        Renderer::Context& Context;
        ImGuiLayer& ImGui;
        Ref<Sampler> Sampler;
        AssetHandle<Shader> CompositeVS;
        AssetHandle<Shader> CompositeFS;
        Ref<PipelineLayout> Layout;
        Ref<GraphicsPipeline> Pipeline;
        Ref<ImageView> ImGuiView;
        TextureHandle SceneHandle;
        TextureHandle ImGuiHandle;
        SamplerHandle SamplerHandle;

        // Imported ids re-declared on every Compile; bound to concrete views per replay.
        ResourceId SwapId;
        ResourceId SceneId;
        ResourceId ImGuiId;

        // The scene output the composite samples. Rebound by SetSceneSource.
        Ref<ImageView> SceneSource;
    };

    Unique<SwapChainCompositePass>
    SwapChainCompositePass::Create(const SwapChainCompositePassInfo& info)
    {
        return Unique<SwapChainCompositePass>(new SwapChainCompositePass(info));
    }

    SwapChainCompositePass::SwapChainCompositePass(const SwapChainCompositePassInfo& info)
        : m_Impl(CreateUnique<Impl>(Impl{.Context = info.Context, .ImGui = info.ImGui}))
    {
        VE_ASSERT(info.SceneSource, "SwapChainCompositePass requires an initial SceneSource");
        m_Impl->SceneSource = info.SceneSource;

        m_Impl->Sampler = Sampler::Create(
            info.Context, {
                              .Name = "SwapChain Composite Sampler",
                              // ClampToEdge prevents sampling garbage past the swapchain extent.
                              .MagFilter = Filter::Linear,
                              .MinFilter = Filter::Linear,
                              .AddressModeU = AddressMode::ClampToEdge,
                              .AddressModeV = AddressMode::ClampToEdge,
                              .AddressModeW = AddressMode::ClampToEdge,
                          });

        const AssetResult<AssetHandle<Shader>> vs = info.Assets.LoadSync<Shader>(FullscreenVertId);
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_Impl->CompositeVS = *vs;

        const AssetResult<AssetHandle<Shader>> fs = info.Assets.LoadSync<Shader>(CompositeFragId);
        VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
        m_Impl->CompositeFS = *fs;

        m_Impl->Layout = PipelineLayout::Create(
            info.Context,
            {
                .Name = "SwapChain Composite Layout",
                .PushConstantRanges =
                    {
                        PushConstantRange::Of<CompositePushConstants>(ShaderStage::Fragment),
                    },
            });

        m_Impl->Pipeline = GraphicsPipeline::Create(
            info.Context,
            {
                .Name = "SwapChain Composite Pipeline",
                .ColorAttachments = {{.Format = info.SwapChainFormat}},
                .PipelineLayout = m_Impl->Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = m_Impl->CompositeVS.Get()->Module},
                        {.Stage = ShaderStage::Fragment,
                         .Module = m_Impl->CompositeFS.Get()->Module},
                    },
            });

        // View over the ImGui layer's rendered output, blended over the scene.
        m_Impl->ImGuiView =
            ImageView::Create(info.Context, {
                                                .Name = "SwapChain Composite Layer View",
                                                .Image = info.ImGui.GetOutputImage(),
                                            });

        BindlessRegistry& bindless = info.Context.GetBindlessRegistry();
        m_Impl->ImGuiHandle = bindless.Register(m_Impl->ImGuiView);
        m_Impl->SamplerHandle = bindless.Register(m_Impl->Sampler);
        m_Impl->SceneHandle = bindless.Register(info.SceneSource);
    }

    SwapChainCompositePass::~SwapChainCompositePass()
    {
        BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
        bindless.Release(m_Impl->SceneHandle);
        bindless.Release(m_Impl->ImGuiHandle);
        bindless.Release(m_Impl->SamplerHandle);
    }

    void SwapChainCompositePass::SetSceneSource(const Ref<ImageView>& sceneSource)
    {
        VE_ASSERT(sceneSource, "SwapChainCompositePass::SetSceneSource requires a non-null view");
        m_Impl->SceneSource = sceneSource;

        // SceneHandle.Index is read live per frame, so re-registering takes effect
        // on the next replay without a recompile.
        BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
        if (m_Impl->SceneHandle.IsValid())
        {
            bindless.Release(m_Impl->SceneHandle);
        }
        m_Impl->SceneHandle = bindless.Register(sceneSource);
    }

    Unique<CompiledGraph> SwapChainCompositePass::Compile(RenderGraph& graph,
                                                          ResourceId swapChainTarget)
    {
        m_Impl->SwapId = swapChainTarget;
        m_Impl->SceneId = graph.Import("SwapChainCompositeScene");
        m_Impl->ImGuiId = graph.Import("SwapChainCompositeLayer");

        graph.AddPass("SwapChain Composite")
            .Color({
                .Resource = m_Impl->SwapId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_Impl->SceneId)
            .Sample(m_Impl->ImGuiId)
            .Execute(
                [this](PassContext& ctx)
                {
                    CommandBuffer& cmd = ctx.Cmd();
                    const uvec2 extent = m_Impl->Context.GetSwapChainExtent();
                    cmd.BindPipeline(m_Impl->Pipeline);
                    cmd.SetViewport({0, 0}, extent);
                    cmd.SetScissor({0, 0}, extent);
                    m_Impl->Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(CompositePushConstants{
                        .SceneTexture = m_Impl->SceneHandle.Index,
                        .ImGuiTexture = m_Impl->ImGuiHandle.Index,
                        .Sampler = m_Impl->SamplerHandle.Index,
                    });
                    cmd.DrawFullscreenTriangle();
                });

        return graph.Compile();
    }

    void SwapChainCompositePass::Execute(CommandBuffer& cmd, CompiledGraph& graph,
                                         const Ref<ImageView>& swapChainView) const
    {
        const RenderGraph::ImportBinding bindings[] = {
            {.Id = m_Impl->SwapId, .View = swapChainView},
            {.Id = m_Impl->SceneId, .View = m_Impl->SceneSource},
            {.Id = m_Impl->ImGuiId, .View = m_Impl->ImGuiView},
        };
        graph.Execute(cmd, bindings);
    }
}
