#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;

    namespace Gui
    {
        class DrawList;
        class RenderTarget;
    }
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ImageView;

    /// @brief Construction parameters for GuiScenePass.
    struct GuiScenePassInfo
    {
        /// @brief Vulkan context for pipeline and resource creation.
        Context& Context;
        /// @brief Asset manager the pass loads its gui shaders through; must outlive the pass.
        AssetManager& Assets;
        /// @brief Native output extent, in pixels — the UI image and composite target size.
        ///
        /// The UI composites at native output resolution (not a render-scale sub-rect) so text
        /// stays sharp.
        uvec2 Extent;
        /// @brief Color format of the scene output the UI blends over and the composite target.
        Format OutputFormat = Format::RGBA16Sfloat;
    };

    /// @brief Records a Gui::DrawList into an offscreen UI image, over a scene output or an HDR target.
    ///
    /// The per-viewport UI stage, modeled on the managed gather + composite tail's shape (not
    /// mounted into it). Each frame the pass uploads a draw list's vertex/index stream into
    /// per-frame ring storage, then records one draw per run — with the run's scissor and pipeline
    /// (rounded-rect SDF or MSDF text) — into a target. It offers two sinks that share the geometry
    /// upload and pipelines:
    ///
    /// - Render blends the recorded UI over a scene output into an owned composite target — the
    ///   screen-space overlay path.
    /// - RenderToTarget records the UI into a supplied persistent Gui::RenderTarget and leaves it
    ///   shader-readable — the render-to-texture path a downstream material samples.
    ///
    /// All draw-list colors are linear by contract and both sinks record in linear space with no
    /// brightness clamp; a color above 1.0 survives into a half-float RenderTarget, and is flattened
    /// only when the composite target holds a lower-precision format.
    ///
    /// Single-owner (Unique); Create is the factory. Owns the UI image, the composite target, the
    /// ring-buffered geometry, and the two gui pipelines. OutputFormat fixes the pipelines' color
    /// format, so a RenderToTarget target must carry that same format.
    class GuiScenePass
    {
    public:
        /// @brief Creates the pass and builds its pipelines, images, and ring buffers.
        /// @param info  Construction parameters.
        /// @return The owning Unique.
        static Unique<GuiScenePass> Create(const GuiScenePassInfo& info);

        /// @brief Releases owned resources through the deferred-destruction path.
        ~GuiScenePass();

        GuiScenePass(const GuiScenePass&) = delete;
        GuiScenePass& operator=(const GuiScenePass&) = delete;

        /// @brief Uploads a draw list's geometry into the current frame's ring region.
        ///
        /// Copies the vertex/index stream into this frame-in-flight's slice of the ring buffers and
        /// caches the run table for the next Render. The draw list is not retained. A geometry
        /// overflow beyond the ring capacity is a fatal assert.
        /// @param drawList  The device-free UI primitives to record.
        void SetDrawList(const Gui::DrawList& drawList);

        /// @brief Resizes the UI image and composite target to a new native output extent.
        ///
        /// Recreates the owned images through the retire path; the pipelines and ring buffers are
        /// unaffected. Invalidates GetOutput() — re-fetch after.
        /// @param extent  The new native output extent, in pixels.
        void Resize(uvec2 extent);

        /// @brief Sets the scale the recorded geometry is magnified by at draw.
        ///
        /// The draw list's positions and clip rects are logical points (the document solved at
        /// extent / scale); the record maps them onto the physical UI image through this factor —
        /// positions via the vertex-stage clip transform, clips via the scissor. 1 (the default)
        /// draws logical points 1:1 with pixels.
        /// @param scale  The UI scale factor; must be positive.
        void SetUiScale(f32 scale);

        /// @brief Records the UI into its image and blends it over the scene output.
        ///
        /// Clears the UI image to transparent, replays each cached run (scissor + pipeline) into it,
        /// then composites the scene output with the UI blended over it into the composite target.
        /// With an empty draw list the composite is the scene output copied through unchanged.
        /// @param cmd          Command buffer to record into.
        /// @param sceneOutput  The viewport's rendered scene output to blend the UI over.
        void Render(CommandBuffer& cmd, const Ref<ImageView>& sceneOutput);

        /// @brief Records the UI into a supplied HDR target and leaves it shader-readable.
        ///
        /// Clears the target to transparent, replays each cached run into it at the target's extent,
        /// then transitions it to a sampleable layout for a later sampler in the frame — the
        /// producer-before-consumer handoff a downstream material's SetTextureHandle consumer reads
        /// across. Distinct from Render: no scene output, no composite, the supplied target is the
        /// whole result. The target's format must match this pass's OutputFormat.
        /// @param cmd     Command buffer to record into.
        /// @param target  The persistent target to record the UI into; left in a shader-read layout.
        void RenderToTarget(CommandBuffer& cmd, Gui::RenderTarget& target);

        /// @brief Returns the composited output view (scene with the UI blended over it).
        ///
        /// Invalidated by Resize — re-fetch after.
        [[nodiscard]] const Ref<ImageView>& GetOutput() const;

        /// @brief Returns the offscreen UI image view (linear premultiplied alpha), before compositing.
        ///
        /// The UI overlay in isolation, for a test that captures the overlay alone. Invalidated by
        /// Resize — re-fetch after.
        [[nodiscard]] const Ref<ImageView>& GetUiImage() const;

    private:
        explicit GuiScenePass(const GuiScenePassInfo& info);

        /// @brief Implementation detail; defined in GuiScenePass.cpp.
        struct Impl;
        /// @brief Pimpl holder.
        Unique<Impl> m_Impl;
    };
}
