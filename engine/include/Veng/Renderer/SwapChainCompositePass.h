#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/RenderGraph.h>

// The fullscreen pass that blends a SceneRenderer's offscreen output behind the
// ImGui layer's rendered overlay into the swapchain. It owns the composite
// pipeline, a sampler, and the three bindless registrations (scene output, ImGui
// output, sampler); Compile adds the composite pass to the app's render graph and
// Execute replays it per frame.
//
// Both inputs are sampled through set-0 bindless inside the graph, so the graph
// derives their barriers. Surfacing a scene output *inside* an ImGui panel — an
// ImGui texture over the output drawn by ImGui::Image, plus the out-of-graph
// sampleability barrier that read needs — is a separate, smaller job a consumer
// does directly against ImGuiLayer and CommandBuffer; this pass is only the
// scene-behind-ImGui swapchain composite.
//
// Windowed-only: it builds on the ImGui + swapchain path, never headless/smoke.
namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ImageView;

    struct SwapChainCompositePassInfo
    {
        Context& Context;
        ImGuiLayer& ImGui;

        // The composite fragment shader is loaded from the engine core pack through
        // this manager; it must outlive the pass.
        AssetManager& Assets;

        // The scene output to composite — the initial SceneRenderer::GetOutput().
        // Rebound through SetSceneSource after Resize/Configure invalidates it.
        Ref<ImageView> SceneSource;

        // The swapchain color format the composite pass writes.
        Format SwapChainFormat;
    };

    class SwapChainCompositePass
    {
    public:
        static Unique<SwapChainCompositePass> Create(const SwapChainCompositePassInfo& info);
        ~SwapChainCompositePass();

        SwapChainCompositePass(const SwapChainCompositePass&) = delete;
        SwapChainCompositePass& operator=(const SwapChainCompositePass&) = delete;

        // Re-register the scene bindless slot after SceneRenderer::Resize/Configure
        // recreates the output. No recompile — the composite reads the scene
        // texture's bindless index live per frame, so the swap takes effect on the
        // next replay.
        void SetSceneSource(const Ref<ImageView>& sceneSource);

        // Add the fullscreen composite pass to `graph` writing `swapChainTarget`,
        // then Compile() it. The recompile seam on swapchain resize.
        [[nodiscard]] Unique<CompiledGraph> Compile(RenderGraph& graph, ResourceId swapChainTarget);

        // Replay the compiled composite graph: bind the per-frame swapchain view plus
        // the pass-owned scene and ImGui-layer views, then execute. The single
        // per-frame call after ImGuiLayer::Render.
        void Execute(CommandBuffer& cmd, CompiledGraph& graph, const Ref<ImageView>& swapChainView) const;

    private:
        explicit SwapChainCompositePass(const SwapChainCompositePassInfo& info);

        struct Impl;
        Unique<Impl> m_Impl;
    };
}
