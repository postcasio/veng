#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/RenderGraph.h>

/// @brief Fullscreen pass that composites a SceneRenderer's output behind ImGui into the swapchain.
///
/// Owns the composite pipeline, a sampler, and the three bindless registrations (scene output,
/// ImGui output, sampler). Compile adds the composite pass to the app's render graph; Execute
/// replays it per frame.
///
/// Both inputs are sampled through set-0 bindless inside the graph, so the graph derives their
/// barriers. Surfacing a scene output inside an ImGui panel (an ImGui::Image plus the
/// out-of-graph sampleability barrier) is a separate consumer responsibility; this pass covers
/// only the scene-behind-ImGui swapchain composite.
///
/// Windowed-only: it builds on the ImGui + swapchain path and is never used headless.
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

    /// @brief Construction parameters for SwapChainCompositePass.
    struct SwapChainCompositePassInfo
    {
        /// @brief Vulkan context for pipeline and resource creation.
        Context& Context;
        /// @brief ImGui layer supplying the overlay output view.
        ImGuiLayer& ImGui;

        /// @brief Asset manager for loading the composite fragment shader from the core pack.
        ///
        /// Must outlive the pass.
        AssetManager& Assets;

        /// @brief Initial scene output to composite — SceneRenderer::GetOutput() at construction.
        ///
        /// Rebound through SetSceneSource after Resize/Configure invalidates the view.
        Ref<ImageView> SceneSource;

        /// @brief Swapchain color format the composite pass writes.
        Format SwapChainFormat;
    };

    /// @brief Composites a scene output and an ImGui overlay into the swapchain.
    ///
    /// Single-owner (Unique); Create is the factory.
    class SwapChainCompositePass
    {
    public:
        /// @brief Creates the pass and builds the composite pipeline.
        static Unique<SwapChainCompositePass> Create(const SwapChainCompositePassInfo& info);
        /// @brief Releases owned resources through the deferred-destruction path.
        ~SwapChainCompositePass();

        SwapChainCompositePass(const SwapChainCompositePass&) = delete;
        SwapChainCompositePass& operator=(const SwapChainCompositePass&) = delete;

        /// @brief Re-registers the scene bindless slot after SceneRenderer::Resize/Configure.
        ///
        /// No recompile needed — the composite reads the bindless index live per frame, so
        /// the swap takes effect on the next replay.
        /// @param sceneSource  The new scene output view from SceneRenderer::GetOutput().
        void SetSceneSource(const Ref<ImageView>& sceneSource);

        /// @brief Adds the fullscreen composite pass to graph and compiles it.
        ///
        /// The recompile seam on swapchain resize.
        /// @param graph            The app's render graph.
        /// @param swapChainTarget  The imported swapchain target this pass writes.
        /// @return The compiled graph ready for per-frame Execute calls.
        [[nodiscard]] Unique<CompiledGraph> Compile(RenderGraph& graph, ResourceId swapChainTarget);

        /// @brief Replays the compiled composite graph for one frame.
        ///
        /// Binds the per-frame swapchain view plus the pass-owned scene and ImGui-layer views,
        /// then executes. The single per-frame call after ImGuiLayer::Render.
        /// @param cmd             Command buffer to record into.
        /// @param graph           The compiled graph returned by Compile.
        /// @param swapChainView   The swapchain image view for this frame.
        void Execute(CommandBuffer& cmd, CompiledGraph& graph, const Ref<ImageView>& swapChainView) const;

    private:
        explicit SwapChainCompositePass(const SwapChainCompositePassInfo& info);

        /// @brief Implementation detail; defined in SwapChainCompositePass.cpp.
        struct Impl;
        /// @brief Pimpl holder.
        Unique<Impl> m_Impl;
    };
}
