#include <Veng/Renderer/ImGuiCompositePass.h>

#include <Veng/Assert.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>

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
        // The composite vertex stage is the engine core pack's fullscreen triangle —
        // shared with SceneRenderer's fullscreen blits — so the pass reuses its id
        // rather than carrying a duplicate VS. The fragment stage is engine-owned.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId CompositeFragId{0x4BCFF3ED0254B42EULL};

        // Selects the composite shader's bindless scene/ImGui texture and sampler
        // slots — set 0 is bound once via BindlessRegistry::Bind; these indices pick
        // the array elements. Matches composite.frag's PushConstants byte-for-byte.
        struct CompositePushConstants
        {
            u32 SceneTexture;
            u32 ImGuiTexture;
            u32 Sampler;
        };
    }

    struct ImGuiCompositePass::Impl
    {
        Renderer::Context& Context;
        ImGuiLayer& ImGui;
        Ref<Sampler> Sampler;
        Ref<ImGuiTexture> SceneTexture;

        // Composite mode only.
        bool Composite = false;
        AssetHandle<Shader> CompositeVS;
        AssetHandle<Shader> CompositeFS;
        Ref<PipelineLayout> Layout;
        Ref<GraphicsPipeline> Pipeline;
        Ref<ImageView> ImGuiView;
        TextureHandle SceneHandle;
        TextureHandle ImGuiHandle;
        SamplerHandle SamplerHandle;

        // The imported ids re-declared on every Compile, bound to their concrete
        // views per replay.
        ResourceId SwapId;
        ResourceId SceneId;
        ResourceId ImGuiId;

        // The scene output the pre-Render barrier transitions and the ImGui texture
        // samples. Rebound by SetSource.
        Ref<ImageView> SceneSource;
    };

    Unique<ImGuiCompositePass> ImGuiCompositePass::Create(const ImGuiCompositePassInfo& info)
    {
        return Unique<ImGuiCompositePass>(new ImGuiCompositePass(info));
    }

    ImGuiCompositePass::ImGuiCompositePass(const ImGuiCompositePassInfo& info)
        : m_Impl(CreateUnique<Impl>(Impl{.Context = info.Context, .ImGui = info.ImGui}))
    {
        VE_ASSERT(info.SceneSource, "ImGuiCompositePass requires an initial SceneSource");

        m_Impl->Composite = info.SwapChainFormat.has_value();
        m_Impl->SceneSource = info.SceneSource;

        m_Impl->Sampler = Sampler::Create(info.Context, {
            .Name = "ImGui Composite Sampler",
            .MagFilter = info.Filter,
            .MinFilter = info.Filter,
            .AddressModeU = info.Wrap,
            .AddressModeV = info.Wrap,
            .AddressModeW = info.Wrap,
        });

        if (m_Impl->Composite)
        {
            const AssetResult<AssetHandle<Shader>> vs = info.Assets.LoadSync<Shader>(FullscreenVertId);
            VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
            m_Impl->CompositeVS = *vs;

            const AssetResult<AssetHandle<Shader>> fs = info.Assets.LoadSync<Shader>(CompositeFragId);
            VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
            m_Impl->CompositeFS = *fs;

            m_Impl->Layout = PipelineLayout::Create(info.Context, {
                .Name = "ImGui Composite Layout",
                .PushConstantRanges = {
                    PushConstantRange::Of<CompositePushConstants>(ShaderStage::Fragment),
                },
            });

            m_Impl->Pipeline = GraphicsPipeline::Create(info.Context, {
                .Name = "ImGui Composite Pipeline",
                .ColorAttachments = {{.Format = *info.SwapChainFormat}},
                .PipelineLayout = m_Impl->Layout,
                .ShaderStages = {
                    {.Stage = ShaderStage::Vertex, .Module = m_Impl->CompositeVS.Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = m_Impl->CompositeFS.Get()->Module},
                },
            });

            // A sampleable view over the ImGui layer's own rendered output, blended
            // over the scene by the composite shader.
            m_Impl->ImGuiView = ImageView::Create(info.Context, {
                .Name = "ImGui Composite Layer View",
                .Image = info.ImGui.GetOutputImage(),
            });

            BindlessRegistry& bindless = info.Context.GetBindlessRegistry();
            m_Impl->ImGuiHandle = bindless.Register(m_Impl->ImGuiView);
            m_Impl->SamplerHandle = bindless.Register(m_Impl->Sampler);
        }

        SetSource(info.SceneSource);
    }

    ImGuiCompositePass::~ImGuiCompositePass()
    {
        if (m_Impl->Composite)
        {
            BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
            bindless.Release(m_Impl->SceneHandle);
            bindless.Release(m_Impl->ImGuiHandle);
            bindless.Release(m_Impl->SamplerHandle);
        }
        // The Ref<ImGuiTexture> destructor calls ImGuiLayer::DestroyTexture itself;
        // dropping the Impl releases it — no explicit destroy here.
    }

    void ImGuiCompositePass::SetSource(const Ref<ImageView>& sceneSource)
    {
        VE_ASSERT(sceneSource, "ImGuiCompositePass::SetSource requires a non-null view");
        m_Impl->SceneSource = sceneSource;

        // The ImGui texture is recreated against the new view in both modes; an
        // ImGuiTexture wraps a fixed view, so the prior one is destroyed.
        m_Impl->SceneTexture = m_Impl->ImGui.CreateTexture(*m_Impl->Sampler, *sceneSource);

        if (m_Impl->Composite)
        {
            // The composite reads m_Impl->SceneHandle.Index live per frame, so
            // re-registering the slot takes effect on the next replay — no recompile.
            BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
            if (m_Impl->SceneHandle.IsValid())
                bindless.Release(m_Impl->SceneHandle);
            m_Impl->SceneHandle = bindless.Register(sceneSource);
        }
    }

    Unique<CompiledGraph> ImGuiCompositePass::Compile(RenderGraph& graph, ResourceId swapChainTarget)
    {
        VE_ASSERT(m_Impl->Composite,
            "ImGuiCompositePass::Compile is composite-mode only (no swapchain composite in panel-only mode)");

        m_Impl->SwapId = swapChainTarget;
        m_Impl->SceneId = graph.Import("ImGuiCompositeScene");
        m_Impl->ImGuiId = graph.Import("ImGuiCompositeLayer");

        graph.AddPass("ImGui Composite")
            .Color({
                .Resource = m_Impl->SwapId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(m_Impl->SceneId)
            .Sample(m_Impl->ImGuiId)
            .Execute([this](PassContext& ctx)
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

    void ImGuiCompositePass::Execute(CommandBuffer& cmd, CompiledGraph& graph,
                                     const Ref<ImageView>& swapChainView) const
    {
        VE_ASSERT(m_Impl->Composite, "ImGuiCompositePass::Execute is composite-mode only");

        const RenderGraph::ImportBinding bindings[] = {
            {m_Impl->SwapId, swapChainView},
            {m_Impl->SceneId, m_Impl->SceneSource},
            {m_Impl->ImGuiId, m_Impl->ImGuiView},
        };
        graph.Execute(cmd, bindings);
    }

    void ImGuiCompositePass::PrepareSceneForImGui(CommandBuffer& cmd) const
    {
        // ImGui's sampled read of the scene output is recorded outside the graph by
        // ImGuiLayer::Render, so no pass .Sample() declaration covers it; transition
        // the output to a sampleable layout here, before that read.
        cmd.PrepareForAccess(m_Impl->SceneSource, AccessKind::Sample);
    }

    ImGuiTexture& ImGuiCompositePass::GetSceneTexture() const
    {
        VE_ASSERT(m_Impl->SceneTexture, "ImGuiCompositePass has no scene texture");
        return *m_Impl->SceneTexture;
    }

    const Ref<ImGuiTexture>& ImGuiCompositePass::GetSceneTextureRef() const
    {
        VE_ASSERT(m_Impl->SceneTexture, "ImGuiCompositePass has no scene texture");
        return m_Impl->SceneTexture;
    }

}
