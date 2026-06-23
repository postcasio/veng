#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ViewportRegion.h>

/// @brief Assembly pass that places each Presented viewport's texture into its region.
///
/// The gather pass scissor-blits a list of placements onto one full-window linear-HDR
/// (RGBA16F) assembly target, in list order, clearing the area no placement covers. That
/// single target is what SwapChainCompositePass consumes — the composite stays a one-source
/// pass and learns nothing about regions. One window-covering placement is the fullscreen
/// game; zero placements is the editor (a cleared target); N quadrant placements is
/// splitscreen — the same gather + composite tail for all three.
///
/// A window-covering placement is a point-sampled same-resolution copy, so the assembled
/// values are bit-identical to sampling the source directly.
///
/// Windowed-only: it feeds the swapchain composite and is never used headless.
namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ImageView;

    /// @brief One Presented viewport's texture and the region it is placed into.
    struct CompositePlacement
    {
        /// @brief The viewport's rendered output, sampled across its whole extent.
        Ref<ImageView> Texture;
        /// @brief The window rectangle the texture is blitted into.
        ViewportRegion Region;
    };

    /// @brief Maximum number of placements one gather may assemble.
    ///
    /// Registering more Presented viewports than this is a fatal assert at register time;
    /// the gather reserves bindless slots up to this bound.
    inline constexpr u32 MaxPresented = 16;

    /// @brief Construction parameters for GatherPass.
    struct GatherPassInfo
    {
        /// @brief Vulkan context for pipeline and resource creation.
        Context& Context;

        /// @brief Asset manager for loading the gather fragment shader from the core pack.
        ///
        /// Must outlive the pass.
        AssetManager& Assets;

        /// @brief Extent of the owned full-window assembly target, in pixels.
        ///
        /// The swapchain extent: placements address regions within it.
        uvec2 Extent;
    };

    /// @brief Assembles Presented viewport textures into one linear-HDR target.
    ///
    /// Single-owner (Unique); Create is the factory. Owns its RGBA16F assembly target and
    /// the bindless slots of the placements it last assembled.
    class GatherPass
    {
    public:
        /// @brief Creates the pass and builds the gather pipeline and assembly target.
        static Unique<GatherPass> Create(const GatherPassInfo& info);
        /// @brief Releases owned resources through the deferred-destruction path.
        ~GatherPass();

        GatherPass(const GatherPass&) = delete;
        GatherPass& operator=(const GatherPass&) = delete;

        /// @brief Rebinds the placement list for the upcoming frames.
        ///
        /// Registers exactly one bindless texture slot per placement (releasing the prior
        /// frame's slots), so a K-placement frame declares exactly K samples and an empty
        /// list declares none. The list is replayed by Execute until the next call.
        /// @param placements  The Presented viewports to assemble, in list order.
        /// @pre placements.size() <= MaxPresented — asserted otherwise.
        void SetPlacements(std::span<const CompositePlacement> placements);

        /// @brief Resizes the owned assembly target to a new window extent.
        ///
        /// Recreates the RGBA16F target; the placement list and the compiled graph are
        /// unaffected. Call from the swapchain-invalidation callback before recompiling.
        /// @param extent  The new window extent in pixels.
        void Resize(uvec2 extent);

        /// @brief The owned assembly target's view, the composite's single source.
        ///
        /// Pass this to SwapChainCompositePassInfo::SceneSource / SetSceneSource. Stable
        /// across SetPlacements; replaced by Resize.
        [[nodiscard]] const Ref<ImageView>& GetOutput() const;

        /// @brief Adds the gather pass to graph and compiles it.
        ///
        /// The recompile seam on swapchain resize.
        /// @param graph  The app's render graph.
        /// @return The compiled graph ready for per-frame Execute calls.
        [[nodiscard]] Unique<CompiledGraph> Compile(RenderGraph& graph);

        /// @brief Replays the compiled gather graph for one frame.
        ///
        /// Transitions each placement texture to Sample (out of graph), then clears the
        /// assembly target and scissor-blits each placement into its region. With an empty
        /// list it writes only the clear color.
        /// @param cmd    Command buffer to record into.
        /// @param graph  The compiled graph returned by Compile.
        void Execute(CommandBuffer& cmd, CompiledGraph& graph) const;

    private:
        explicit GatherPass(const GatherPassInfo& info);

        /// @brief Implementation detail; defined in GatherPass.cpp.
        struct Impl;
        /// @brief Pimpl holder.
        Unique<Impl> m_Impl;
    };
}
