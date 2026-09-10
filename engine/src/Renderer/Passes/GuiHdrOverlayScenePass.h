#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GuiScenePass;

    /// @brief Blends scene-HDR-pre-bloom GUI overlays into the scene color at the pre-bloom tail anchor.
    ///
    /// The net-new tail-anchor GUI pass: it composites each SceneHdrPreBloom GuiOverlay the engine
    /// drove this frame (conveyed on SceneView::HdrOverlays) into the finished scene color in place,
    /// with a load (blend-over), so the overlay is tonemapped and blooms with the scene at output
    /// resolution. It reuses GuiScenePass's linear/unclamped HDR fragment/blend path — an owned
    /// GuiScenePass records the merged geometry — while the world-anchored vertex projection is
    /// net-new: each overlay's draw list is either scaled flat to the scene-color region
    /// (screen-space) or projected through the live camera onto its flat virtual plane
    /// (world-anchored), and the results merge into one screen-space draw list a single record draws.
    ///
    /// The renderer inserts this after any post-process effect passes and before bloom, and points it
    /// at the resolved scene-color id (the effect chain's output, or the raw HDR when no effect ran),
    /// so bloom reads the overlay's output.
    class GuiHdrOverlayScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and its owned GuiScenePass recorder.
        /// @param context       The render context for pipeline and resource creation.
        /// @param assets        The asset manager the recorder loads its gui shaders through.
        /// @param outputFormat  Color format of the scene-color target this pass blends into.
        /// @param extent        The allocation extent (informational; the record uses PostResolveExtent).
        GuiHdrOverlayScenePass(Context& context, AssetManager& assets, Format outputFormat,
                               uvec2 extent);

        /// @brief Releases the owned recorder.
        ~GuiHdrOverlayScenePass() override;

        GuiHdrOverlayScenePass(const GuiHdrOverlayScenePass&) = delete;
        GuiHdrOverlayScenePass& operator=(const GuiHdrOverlayScenePass&) = delete;

        /// @brief Sets the scene-color id this pass loads and blends the overlays into (per Rebuild).
        /// @param sceneColor  The resolved scene-color id (effect-chain output, or the raw HDR).
        void SetOutput(ResourceId sceneColor) { m_Output = sceneColor; }

        /// @brief Updates the cached allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Contributes the load-blend overlay pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The allocation extent (the record maps to the frame's PostResolveExtent).
        uvec2 m_Extent;
        /// @brief The scene-color id this pass loads and stores (blended over in place).
        ResourceId m_Output;
        /// @brief The owned recorder supplying GuiScenePass's geometry rings, pipelines, and blend.
        Unique<GuiScenePass> m_Gui;
        /// @brief The per-frame merged screen-space draw list every overlay projects into.
        Gui::DrawList m_Merged;
    };
}
