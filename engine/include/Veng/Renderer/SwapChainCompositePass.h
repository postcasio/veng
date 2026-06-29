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

        /// @brief Initial single source to composite — the gather pass's assembly target.
        ///
        /// GatherPass::GetOutput() at construction: the full-window linear-HDR target the
        /// gather pass assembled the Presented viewports into. Rebound through SetSceneSource
        /// after a resize invalidates the view.
        Ref<ImageView> SceneSource;

        /// @brief Swapchain color format the composite pass writes.
        Format SwapChainFormat;

        /// @brief Resolved swapchain color space, selecting the final transfer encoding.
        ///
        /// SrgbNonlinear and ExtendedLinearSrgb write the blended linear color unencoded;
        /// Hdr10St2084 converts primaries and PQ-encodes. From Context::GetActiveDisplayColorSpace().
        DisplayColorSpace ColorSpace = DisplayColorSpace::SrgbNonlinear;

        /// @brief HDR10 reference (paper) white in nits: the display luminance of linear 1.0.
        ///
        /// Used only when ColorSpace is Hdr10St2084.
        f32 PaperWhiteNits = 200.0f;

        /// @brief HDR10 peak luminance in nits the encoded signal is clamped to.
        ///
        /// Used only when ColorSpace is Hdr10St2084.
        f32 PeakNits = 1000.0f;
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

        /// @brief Re-views and re-registers the ImGui overlay after the ImGui layer recreates it.
        ///
        /// The ImGui layer recreates its offscreen image on swapchain resize, retiring the one the
        /// composite viewed at construction; without this the composite keeps sampling the stale
        /// image (old size → squished, old content → frozen). Like SetSceneSource, the bindless
        /// index is read live per frame, so no recompile is needed. Call from the
        /// swapchain-invalidation callback, after the ImGui layer's own callback has recreated it.
        void RefreshImGuiSource();

        /// @brief Re-targets the composite at a re-negotiated swapchain format and color space.
        ///
        /// Updates the display-transfer encoding and, when the format changed, rebuilds the
        /// composite pipeline. Call from the swapchain-invalidation callback (the surface can
        /// switch color space when a window moves to a display with different HDR support),
        /// before recompiling the graph. A no-op when neither changed.
        /// @param swapChainFormat  The current swapchain format, from Context::GetSwapChainFormat().
        /// @param colorSpace       The resolved color space, from Context::GetActiveDisplayColorSpace().
        void SetSwapChainTarget(Format swapChainFormat, DisplayColorSpace colorSpace);

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
        void Execute(CommandBuffer& cmd, CompiledGraph& graph,
                     const Ref<ImageView>& swapChainView) const;

    private:
        explicit SwapChainCompositePass(const SwapChainCompositePassInfo& info);

        /// @brief Builds the composite graphics pipeline for the given swapchain format.
        /// @param swapChainFormat  Color format the composite pass writes.
        void RebuildPipeline(Format swapChainFormat);

        /// @brief Implementation detail; defined in SwapChainCompositePass.cpp.
        struct Impl;
        /// @brief Pimpl holder.
        Unique<Impl> m_Impl;
    };
}
