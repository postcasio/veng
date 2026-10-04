#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../GuiOverlayProjection.h"
#include "PerViewMaterial.h"

namespace Veng
{
    class AssetManager;
    class Material;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class GuiScenePass;
    class GraphicsPipeline;
    struct GuiHdrOverlayView;

    /// @brief Composites scene-HDR-pre-bloom GUI overlays into the scene color at the pre-bloom anchor.
    ///
    /// The tail-anchor GUI pass: it composites each SceneHdrPreBloom GuiOverlay the engine drove this
    /// frame (conveyed on SceneView::HdrOverlays) into the finished scene color in place, so an overlay
    /// is tonemapped and blooms with the scene at output resolution. It runs two paths, by whether an
    /// overlay names a composite material:
    ///
    /// - **Direct** (no material) — the overlays merge into one screen-space draw list a single record
    ///   blends premultiplied-over into the scene color. A screen-space overlay scales its logical
    ///   extent to the region; a world-anchored one projects each vertex through the live camera. The
    ///   pass is declared only while a direct overlay is conveyed (SetHasDirect), so a frame of only
    ///   material overlays pays no load and store of the scene color for it.
    /// - **Material** — an overlay naming a MaterialInstance is composited through it: its document is
    ///   rendered to a renderer-owned intermediate HDR target, then the material runs as a composite
    ///   that reads the intermediate and writes shaped color into the scene HDR (premultiplied-over)
    ///   and, when the material declares a bloom mask, an amplitude into the renderer's bloom-mask
    ///   target (additive) — so the overlay can bloom by a strength it names, decoupled from its drawn
    ///   luminance (the glow-split the fixed bloom bright-pass cannot express).
    ///
    /// A material overlay costs what its document covers, not the screen. PrepareDocuments projects
    /// each document ahead of the graph and takes the granule-rounded bounds of the projection as its
    /// **document rect**; the document renders into the intermediate with the rect's origin at the
    /// intermediate's origin, and the composite is scissored to the rect. The composite material reads
    /// its document through `LoadOverlayDocument` (`Veng/overlay_composite.slang`) with the rect the
    /// pass writes into its `DocumentRect` field; a material without that field is reported once and
    /// not composited, rather than drawn reading the wrong texels.
    ///
    /// Material overlays share **one reused intermediate**, so each is a render-to-intermediate pass
    /// followed by its composite, declared in order — the graph serializes the reuse. The count of
    /// material overlays is a structural quantity: the renderer recompiles the pass set when it changes
    /// (SetCompositeCount), so a frame's material overlays each get their declared pass pair.
    ///
    /// The renderer inserts this after any post-process effect passes and before bloom, and points it
    /// at the resolved scene-color id (the effect chain's output, or the raw HDR when no effect ran),
    /// so bloom reads the overlays' output.
    ///
    /// The intermediate is renderer-owned, so its bindless slot is a per-view value; a composite
    /// therefore writes it into this renderer's own PerViewMaterial mirror of the overlay's material
    /// and draws through that, since two viewports presenting one world carry the same instance
    /// pointer on their overlays.
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

        /// @brief Wires the material-composite targets (per Rebuild).
        ///
        /// A no-op set (an invalid doc target) leaves only the direct path. The bloom-mask id is valid
        /// only when bloom is active; when invalid, the composite pipelines drop the mask attachment.
        /// The intermediate's bindless slot and extent arrive separately (SetDocTarget), since a grow
        /// changes them without a Rebuild.
        /// @param docTarget        The renderer-owned intermediate HDR target the document renders to.
        /// @param bloomMask        The tail-side bloom-mask target, or invalid when bloom is off.
        /// @param bloomMaskFormat  The bloom-mask target's format.
        void SetComposite(ResourceId docTarget, ResourceId bloomMask, Format bloomMaskFormat);

        /// @brief Sets how many material overlays are composited this Rebuild (declares that many pass pairs).
        /// @param count  The number of SceneHdrPreBloom overlays naming a material.
        void SetCompositeCount(u32 count) { m_CompositeCount = count; }

        /// @brief Sets whether any overlay without a material is conveyed (declares the direct pass).
        /// @param hasDirect  True when at least one SceneHdrPreBloom overlay names no material.
        void SetHasDirect(bool hasDirect) { m_HasDirect = hasDirect; }

        /// @brief Points the material path at the intermediate's current bindless slot and extent.
        ///
        /// Called whenever the renderer reallocates the intermediate (a grow), which changes both
        /// without a recompile: the graph resolves the imported target's view per frame.
        /// @param docTargetHandle  The intermediate's bindless slot.
        /// @param docTargetExtent  The intermediate's pixel extent.
        void SetDocTarget(TextureHandle docTargetHandle, uvec2 docTargetExtent)
        {
            m_DocTargetHandle = docTargetHandle;
            m_DocTargetExtent = docTargetExtent;
        }

        /// @brief Projects this frame's material-overlay documents and derives each one's document rect.
        ///
        /// Runs once per Execute, ahead of the graph, so the renderer can grow the intermediate to
        /// the largest rect before the document passes render into it.
        /// @param view  The frame's resolved scene view (its HdrOverlays and PostResolveExtent).
        /// @return The largest document rect size this frame; zero when no document covers a pixel.
        [[nodiscard]] uvec2 PrepareDocuments(const SceneView& view);

        /// @brief Updates the cached allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Contributes the direct-blend and material-composite passes into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Records the @p index-th material overlay's document into the intermediate target.
        void RecordDocument(const ScenePassContext& ctx, u32 index);
        /// @brief Writes the document's frame into the material's optional fields that ask for it.
        ///
        /// `DocumentExtent` (the document's logical extent), `SceneFromDocument` (document points to
        /// homogeneous scene pixels) and `DocumentFromScene` (its inverse), each a 3x3 as three
        /// `float4` rows; a field the material does not declare is skipped.
        static void WriteDocumentFrame(MaterialInstance& material, const GuiHdrOverlayView& overlay,
                                       const SceneView& view);
        /// @brief Composites the @p index-th material overlay's document through its material.
        void RecordComposite(const ScenePassContext& ctx, u32 index);
        /// @brief Returns the @p index-th overlay naming a material this frame, or nullptr.
        const GuiHdrOverlayView* MaterialOverlay(const SceneView& view, u32 index) const;
        /// @brief Returns (building on demand) the composite pipeline for @p material's parent.
        const Ref<GraphicsPipeline>& CompositePipeline(const MaterialInstance& material);
        /// @brief Appends @p overlay's draw list to @p into, projected into the view's target pixels.
        /// @return False when the projection culled the document (a vertex behind the eye).
        static bool AppendOverlay(Gui::DrawList& into, const GuiHdrOverlayView& overlay,
                                  const SceneView& view);

        /// @brief The render context, held for composite-pipeline creation.
        Context& m_Context;
        /// @brief The asset manager, held for the per-view material mirrors.
        AssetManager& m_Assets;
        /// @brief The allocation extent (the record maps to the frame's PostResolveExtent).
        uvec2 m_Extent;
        /// @brief Color format of the scene-color target, for the composite color attachment.
        Format m_OutputFormat;
        /// @brief The scene-color id this pass loads and stores (blended over in place).
        ResourceId m_Output;
        /// @brief The renderer-owned intermediate HDR target the document renders to before compositing.
        ResourceId m_DocTargetId;
        /// @brief The intermediate's bindless slot, written into each material's Document field each frame.
        TextureHandle m_DocTargetHandle;
        /// @brief The intermediate's pixel extent (at least the largest document rect this frame).
        uvec2 m_DocTargetExtent{0, 0};
        /// @brief The post-resolve-allocation bloom-mask target, or invalid when bloom is off.
        ///
        /// Always the mask on this pass's own side of the promotion — the promoted target while the
        /// scene rasterizes below the post-resolve allocation, the renderer's own otherwise — so the
        /// composite's two attachments are the same size.
        ResourceId m_BloomMaskId;
        /// @brief The bloom-mask target's format.
        Format m_BloomMaskFormat = Format::Undefined;
        /// @brief How many material overlays are composited (declared pass pairs) this Rebuild.
        u32 m_CompositeCount = 0;
        /// @brief Whether an overlay without a material is conveyed (the direct pass is declared).
        bool m_HasDirect = false;
        /// @brief The owned recorder supplying GuiScenePass's geometry rings, pipelines, and blend.
        Unique<GuiScenePass> m_Gui;
        /// @brief The per-frame merged/projected screen-space draw list a record draws from.
        Gui::DrawList m_Merged;
        /// @brief Each material overlay's document projected into target pixels this frame.
        ///
        /// Filled by PrepareDocuments, indexed as MaterialOverlay indexes them; a document record
        /// replays its entry shifted by the rect's origin.
        vector<Gui::DrawList> m_DocLists;
        /// @brief Each material overlay's document rect this frame, parallel to m_DocLists.
        vector<GuiOverlayDocumentRect> m_DocRects;
        /// @brief Parent materials already reported for lacking a DocumentRect field (reported once).
        set<const Material*> m_ReportedWithoutRect;
        /// @brief One mirror per composited overlay, indexed as MaterialOverlay indexes them.
        ///
        /// A slot rebuilds its mirror when the overlay at that index names a different instance, so
        /// the set is bounded by the frame's composite count rather than by every instance seen.
        vector<PerViewMaterial> m_ViewMaterials;
        /// @brief One composite pipeline per parent Material, keyed by parent; rebuilt on a mask-presence flip.
        map<const Material*, Ref<GraphicsPipeline>> m_CompositePipelines;
        /// @brief Whether the cached composite pipelines were built with the mask attachment.
        bool m_PipelinesHaveMask = false;
    };
}
