#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;

    namespace Gui
    {
        class DrawList;
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

    /// @brief Records a Gui::DrawList into an offscreen UI image and blends it over a scene output.
    ///
    /// The per-viewport UI overlay stage, modeled on the managed gather + composite tail's shape
    /// (not mounted into it). Each frame the pass uploads a draw list's vertex/index stream into
    /// per-frame ring storage, records one draw per run — with the run's scissor and pipeline
    /// (rounded-rect SDF or MSDF text) — into an owned linear premultiplied-alpha UI image, then
    /// blends that image over the viewport's scene output into an owned composite target. All
    /// draw-list colors are linear by contract and the blend runs in linear space, so the UI
    /// composites correctly ahead of the display-transfer encode.
    ///
    /// Single-owner (Unique); Create is the factory. Owns the UI image, the composite target, the
    /// ring-buffered geometry, and the two gui pipelines.
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

        /// @brief Records the UI into its image and blends it over the scene output.
        ///
        /// Clears the UI image to transparent, replays each cached run (scissor + pipeline) into it,
        /// then composites the scene output with the UI blended over it into the composite target.
        /// With an empty draw list the composite is the scene output copied through unchanged.
        /// @param cmd          Command buffer to record into.
        /// @param sceneOutput  The viewport's rendered scene output to blend the UI over.
        void Render(CommandBuffer& cmd, const Ref<ImageView>& sceneOutput);

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
