#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/DrawBudgetStats.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Renderer/VolumeMarch.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneBroadphase.h>

#include <array>
#include <span>

/// @brief Long-lived, configurable deferred render pipeline.
///
/// Owns an offscreen target, renders a Scene from a Camera through an internal
/// compiled RenderGraph composed of reusable ScenePass units, and hands back a
/// sampleable result.
///
/// Surface lifetime split by how often each piece of state changes:
/// - Create: allocate persistent resources and compile the graph.
/// - Resize: recreate extent-sized resources and recompile.
/// - Configure: recreate affected resources and recompile topology.
/// - Execute: replay the graph per frame — no reallocation or recompile.
/// - GetOutput: return the owned sampleable result.
namespace Veng
{
    class Scene;
    class AssetManager;
    class Material;
    class MaterialInstance;
    class EnvironmentMap;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ScenePass;
    class ShadowScenePass;
    class PunctualShadowScenePass;
    class ShadowSystem;
    class BloomPyramid;
    class AutoExposureMeter;
    class TaaResolve;
    class SsrChain;
    class DofChain;
    class RefractionGrab;
    class HalfResTranslucency;
    class GpuCullSystem;
    class PickingSystem;
    class Image;
    class Sampler;
    class Buffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class SkyResolver;
    class PointField;
    struct DebugBlitPipelines;
    struct FrameTopology;

    /// @brief Long-lived deferred render pipeline owning an offscreen target.
    ///
    /// Single-owner (Unique); Create is the factory. See the namespace-level doc
    /// for the lifetime-split surface (Create/Resize/Configure/Execute/GetOutput).
    class SceneRenderer
    {
    public:
        /// @brief Creates a SceneRenderer and compiles its initial render graph.
        static Unique<SceneRenderer> Create(const SceneRendererInfo& info);
        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        /// @brief Recreates the extent-sized output and recompiles the internal graph.
        ///
        /// Invalidates the Ref a prior GetOutput() returned. A consumer caching a
        /// bindless TextureHandle or ImGui texture from it must re-fetch and re-register
        /// after this call.
        /// @param extent  New render extent in pixels.
        void Resize(uvec2 extent);

        /// @brief Recreates affected resources and recompiles the graph's topology.
        ///
        /// Invalidates the prior GetOutput() Ref like Resize. ShadowResolution and
        /// PunctualShadowResolution are clamped to GetMaxShadowResolution() /
        /// GetMaxPunctualShadowResolution() before any atlas is sized, so an over-large
        /// request degrades to the largest valid atlas rather than a fatal driver error.
        /// @param settings  New topology and sizing knobs.
        void Configure(const SceneRendererSettings& settings);

        /// @brief Largest directional-cascade tile resolution this device supports.
        ///
        /// The directional atlas tiles its cascades in a grid at most two tiles per
        /// side (2×2 at four cascades), so a tile larger than
        /// Context::GetMaxImageDimension2D() / 2 would overflow the device's image
        /// limit. Configure() clamps ShadowResolution to this; a UI sizing the knob
        /// uses it as the slider maximum.
        /// @return The maximum valid ShadowResolution, in texels.
        [[nodiscard]] u32 GetMaxShadowResolution() const;

        /// @brief Largest punctual-atlas tile resolution this device supports.
        ///
        /// The punctual atlas tiles CubeFaceCount columns × MaxShadowedPunctual rows,
        /// so its widest dimension is CubeFaceCount × resolution; a tile larger than
        /// Context::GetMaxImageDimension2D() / CubeFaceCount would overflow the
        /// device's image limit. Configure() clamps PunctualShadowResolution to this;
        /// a UI sizing the knob uses it as the slider maximum.
        /// @return The maximum valid PunctualShadowResolution, in texels.
        [[nodiscard]] u32 GetMaxPunctualShadowResolution() const;

        /// @brief Replays the internal graph against this frame's view.
        ///
        /// Records each pass unit's draws. Never reallocates or recompiles.
        /// @param cmd   Command buffer to record into.
        /// @param view  Per-frame scene input; the renderer overwrites its output fields.
        void Execute(CommandBuffer& cmd, const SceneView& view);

        /// @brief Returns the sampleable view of the owned result.
        ///
        /// The image is allocated at the high-water-mark extent; under dynamic resolution only
        /// its top-left GetValidExtent() sub-rect holds this frame's rendered content. A consumer
        /// upscales that sub-rect (see GetValidExtent). Invalidated by Resize and Configure;
        /// re-fetch after those calls.
        [[nodiscard]] Ref<ImageView> GetOutput() const;

        /// @brief Returns the valid sub-rect extent of the output from the last Execute.
        ///
        /// round(allocExtent * SceneView::RenderScale) from the last Execute, clamped to
        /// [1, allocExtent]. The output image (GetOutput) is allocated at the full extent; only
        /// the top-left GetValidExtent() texels are this frame's content, and a consumer sampling
        /// it must remap its UVs into [0, GetValidExtent()/allocExtent] to upscale (a half-texel
        /// inset avoids bleeding past the valid edge). Equal to the allocated extent before the
        /// first Execute and whenever RenderScale is 1.0.
        [[nodiscard]] uvec2 GetValidExtent() const;

        /// @brief Returns the total resident per-submesh candidate count from the last Execute.
        ///
        /// One per submesh of every (Transform, MeshRenderer) with a loaded mesh, before any
        /// frustum cull. Zero before the first Execute.
        [[nodiscard]] u32 GetLastVisibleCount() const;

        /// @brief Returns the number of submesh candidates that survived the camera-frustum cull in the last Execute.
        ///
        /// The BVH frustum descent's survivor count — one per per-submesh candidate the
        /// camera frustum kept, always <= GetLastVisibleCount(). A materialless or
        /// not-yet-resident survivor still counts. Under CullMode::GPU this is the count
        /// uploaded to the cull compute pass. The middle stage of the gathered →
        /// frustum-survived → drawn funnel. Zero before the first Execute.
        [[nodiscard]] u32 GetFrustumSurvivedCount() const;

        /// @brief Returns the per-submesh count the g-buffer pass drew in the last Execute.
        ///
        /// Equals GetFrustumSurvivedCount() — it counts frustum survivors, not draw slots laid
        /// out (a materialless or not-yet-resident survivor counts even though it records no
        /// command, and a frame clamped by the per-frame draw-slot budget still reports every
        /// survivor). What the budget actually granted, and what it dropped, is
        /// GetDrawBudgetStats(). Under CullMode::GPU the occlusion stage shows up separately as
        /// GetLastGpuSurvivorCount() (the device-side draws after the hi-Z test zeros occluded
        /// commands). Zero before the first Execute.
        [[nodiscard]] u32 GetLastDrawnCount() const;

        /// @brief Returns the per-frame draw-budget accounting from the last Execute.
        ///
        /// The slot limit in force, the slots the three gather phases claimed, and the submeshes
        /// each phase could not draw once a budget was exhausted. A frame within budget reports
        /// zero drops; a frame over it is clamped rather than failed, and this is the number a
        /// consumer profiling a heavy scene reads (the accompanying warning fires only once per
        /// renderer). All zero before the first Execute.
        /// @return The last Execute's draw-budget statistics.
        [[nodiscard]] DrawBudgetStats GetDrawBudgetStats() const;

        /// @brief Returns the aggregate point-field draw statistics from the last Execute.
        ///
        /// The point-field pass's per-frame counters — fields walked, cells in-frustum / measured,
        /// resolved sprite draws issued and points submitted, and aggregate splats drawn — summed
        /// across every field. The point-field analogue of the mesh cull funnel
        /// (GetLastVisibleCount / GetFrustumSurvivedCount / GetLastDrawnCount): a consumer profiling
        /// a heavy field reads the sprite/splat split here instead of GPU timestamps. All zero when
        /// no point-field pass is active or before the first Execute that drew a field.
        /// @return The last Execute's point-field draw statistics.
        [[nodiscard]] PointFieldStats GetPointFieldStats() const;

        /// @brief Forces the point-field pass onto the direct sprite path (A/B verification hook).
        ///
        /// Bypasses the per-point compute expansion so both paths can be captured and compared; a
        /// no-op when no point-field pass is active or on a device without the compute path. The
        /// compute and direct paths draw a surviving point bit-comparably (modulo record f16
        /// quantization), so this is the reference the automatic selection is checked against.
        /// @param force True to draw every field direct; false to restore automatic selection.
        void SetPointFieldForceDirect(bool force);

        /// @brief Returns the topology/sizing settings in effect, as of the last Create/Configure.
        ///
        /// The requested settings (shadow resolutions clamped to the device caps at apply time);
        /// a settings editor reads its starting state from here rather than mirroring a copy.
        [[nodiscard]] const SceneRendererSettings& GetSettings() const { return m_Settings; }

        /// @brief Returns the cull mode actually in effect, after the device-support fallback.
        ///
        /// Equals Settings.Cull when CullMode::GPU is requested and
        /// Context::IsGpuDrivenCullingSupported() is true; otherwise CullMode::CPU. Reflects the
        /// last Configure/Create.
        [[nodiscard]] SceneRendererSettings::CullMode GetActiveCullMode() const;

        /// @brief Returns the GPU cull's survivor count read back from the previous Execute.
        ///
        /// Under CullMode::GPU this is the number of candidates whose instanceCount the cull wrote
        /// 1 (the draws the indirect submission actually issued), read back one frame late so it
        /// never gates the draw. Zero under CullMode::CPU and before the second GPU Execute.
        [[nodiscard]] u32 GetLastGpuSurvivorCount() const;

        /// @brief Reads back the GPU cull's per-candidate instanceCount verdicts from the last Execute.
        ///
        /// One entry per camera-frustum survivor candidate (in dispatch order), each 1 (drawn) or 0
        /// (occluded), downloaded from the indirect command buffer. Blocks on a device read; exposed
        /// for the GPU↔CPU set-equivalence test. Empty under CullMode::CPU.
        /// @return The per-candidate instanceCount verdicts, or empty if no GPU Execute has run.
        [[nodiscard]] vector<u32> ReadbackGpuSurvivorFlags() const;

        /// @brief Returns true if the broadphase rebuilt its tree during the most recent Execute.
        ///
        /// False on a fully static frame (the scene's spatial version was unchanged).
        /// Diagnostic only; the rendered image is identical regardless.
        [[nodiscard]] bool DidBroadphaseRebuildLastFrame() const;

        /// @brief Returns true if the atmosphere LUTs regenerated during the most recent Execute.
        ///
        /// True only on a frame the Atmosphere parameters changed (or the first frame the
        /// atmosphere sky was active) — the once-per-change contract. Diagnostic only.
        [[nodiscard]] bool DidRegenerateAtmosphereLastFrame() const;

        /// @brief Returns the number of nodes in the broadphase BVH (internal + leaf).
        ///
        /// Diagnostic only. Zero before the first Execute or with no resident candidates.
        [[nodiscard]] u32 GetBroadphaseNodeCount() const;

        /// @brief Returns the g-buffer albedo (G0) view.
        ///
        /// Renderer-owned; invalidated by Resize. Exposed for tests and tooling; normal
        /// consumers read only GetOutput().
        [[nodiscard]] Ref<ImageView> GetAlbedoView() const;
        /// @brief Returns the g-buffer world-normal (G1) view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetNormalView() const;
        /// @brief Returns the g-buffer packed ORM (G2) view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetOrmView() const;
        /// @brief Returns the depth buffer view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetDepthView() const;

        /// @brief Returns the whole-chain sampled view of the hi-Z depth pyramid.
        ///
        /// The max-Z mip chain reduced from the depth target each Execute. Renderer-owned and
        /// persisted across frames; invalidated by Resize and Configure. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetHiZView() const;

        /// @brief Returns the storage view of hi-Z mip @p level (one mip per view).
        ///
        /// Exposed for tests reading back a single reduced mip. Invalidated by Resize and Configure.
        /// @param level  Mip level in [0, mip count).
        [[nodiscard]] Ref<ImageView> GetHiZMipView(u32 level) const;

        /// @brief Returns the number of mip levels in the hi-Z pyramid.
        [[nodiscard]] u32 GetHiZMipCount() const;

        /// @brief Returns whether the previous-frame pyramid is valid to occlusion-test against this frame.
        ///
        /// False on the first Execute, the Execute immediately after a Resize/Configure
        /// recreated the pyramid, and on a detected large view delta (translation past a
        /// fraction of the scene diagonal, forward-axis rotation past the threshold, or
        /// any projection change). When false the GPU cull skips occlusion (frustum-only),
        /// so stale or absent history can only leave a draw in, never wrongly cull it.
        /// Reflects the most recent Execute; defaults false before the first.
        [[nodiscard]] bool IsHiZHistoryValid() const;

        /// @brief Returns the camera world->clip matrix Execute captured last frame.
        ///
        /// The occlusion test screen-bounds a candidate against the previous-frame pyramid,
        /// so it must use the previous-frame view-projection (decision 2). Identity before
        /// the first Execute. Valid to test against only when IsHiZHistoryValid() is true.
        [[nodiscard]] mat4 GetPreviousViewProj() const;

        /// @brief Returns the HDR target the deferred lighting pass writes before tonemap.
        ///
        /// Exposed for tests and tooling. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetHdrView() const;

        /// @brief Returns the bloom composite result the tonemap stage reads when Bloom is on.
        ///
        /// Null when Bloom is off (tonemap reads the raw HDR target instead). Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetBloomResultView() const;

        /// @brief Returns the persisted TAA history target, or null when TAA is off.
        ///
        /// Holds the previous frame's resolved HDR. Renderer-owned; invalidated by Resize
        /// and Configure. Exposed for tests inspecting temporal accumulation.
        [[nodiscard]] Ref<ImageView> GetTaaHistoryView() const;

        /// @brief Returns the per-object velocity target (g-buffer channel G3).
        ///
        /// RG screen-space motion vectors written by the surface pass as a fourth g-buffer
        /// channel every frame (not a separate prepass, never null). Renderer-owned;
        /// invalidated by Resize and Configure. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetVelocityView() const;

        /// @brief Returns the punctual shadow atlas view (set 1 binding 4).
        ///
        /// A 2D depth atlas of MaxShadowedPunctual·CubeFaceCount tiles, SampleCmp'd by the
        /// lighting pass. Renderer-owned; invalidated by Resize and Configure. Exposed for
        /// the render-pass handoff and for tests inspecting the atlas extent.
        [[nodiscard]] Ref<ImageView> GetPunctualShadowView() const;

        /// @brief Returns the immediate-mode debug-draw accumulator for this renderer.
        ///
        /// A caller pushes lines/billboards each frame; the DebugDrawScenePass flushes them when
        /// SceneRendererSettings::DebugDraw is on. The accumulator clears at the start of every
        /// Execute, so a primitive is re-pushed each frame it should appear. A mutable reference
        /// from a const method by the Native-idiom rule: the renderer's constness is its own
        /// identity, not the per-frame accumulator state.
        /// @return The renderer-owned DebugDraw accumulator.
        [[nodiscard]] DebugDraw& GetDebugDraw() const;

        /// @brief Records a pending pick at a render-target texel, serviced by the next Execute(s).
        ///
        /// The next Execute that runs the picking pass copies the (2*Picking::SearchRadius+1)²
        /// texel neighborhood around @p texel out of the id target into a host-visible staging
        /// buffer, on the graphics queue; the result becomes readable through PollPickId() once that
        /// frame's GPU work has completed (a frame or two later — never a WaitIdle). A request issued
        /// while one is already in flight replaces it. A no-op when SceneRendererSettings::Picking is
        /// not set.
        /// @param texel  The render-target texel to pick, in allocation pixels (top-left origin).
        /// @pre SceneRendererSettings::Picking is set on this renderer.
        void RequestPick(uvec2 texel);

        /// @brief Returns true when a pick request has been issued but not yet resolved or polled.
        ///
        /// Covers the window between RequestPick() and the PollPickId() that consumes the result.
        [[nodiscard]] bool IsPickInFlight() const;

        /// @brief Returns the resolved pick id once a requested pick's readback is ready, else nullopt.
        ///
        /// Applies the screen-space search radius to the staged neighborhood: the exact cursor texel
        /// wins when non-zero; otherwise the nearest non-zero id to the cursor. Returns the raw pick
        /// id (packed entity index + 1, or Picking::NoEntityId for background). Returns nullopt while
        /// the readback is still in flight (the staged frame has not completed). Consuming the result
        /// clears the in-flight state, so a caller polls each frame until it returns a value.
        /// @return The resolved pick id when ready; nullopt while the readback is still pending.
        [[nodiscard]] optional<u32> PollPickId();

    private:
        explicit SceneRenderer(const SceneRendererInfo& info);

        /// @brief Recreates the owned output image and view at the current extent and format.
        void CreateOutput();
        /// @brief Recreates g-buffer images/views at the current extent and (re-)registers them into bindless.
        void CreateGBuffer();
        /// @brief Loads the baked LTC lookup tables from the core pack into textures and registers them.
        void CreateLtcResources();
        /// @brief Recreates the HDR image/view at the current extent and (re-)registers it into bindless.
        void CreateHdr();
        /// @brief Recreates the bloom-mask image/view at the current extent and (re-)registers it into bindless.
        void CreateBloomMask();
        /// @brief Builds the engine-owned fullscreen pipelines and loads the core PostProcess materials.
        ///
        /// Called once at Create; the lighting pipeline writes the HDR format, the debug-blit pipelines
        /// write the output format.
        void CreatePipelines();

        /// @brief Rebuilds the pass set from Settings.Mode and recompiles the RenderGraph.
        void Rebuild();

        /// @brief Resolves the scene's PointField components into this Execute's live field set.
        ///
        /// Walks View<PointField> off @p view.World, applies each component's authored Lod to its
        /// built field, and collects every live (non-null, non-empty) Renderer::PointField into
        /// m_PointFields for the point-field pass to draw — the lights model. When whether any live
        /// field exists changes between Executes, drives an internal Rebuild to insert or drop the
        /// pass at the frame boundary (reusing the imported output, so GetOutput() stays valid).
        /// @param view  The scene to resolve the point fields from.
        void ResolvePointFields(const SceneView& view);

        /// @brief Resolves the scene's VolumeField components into this Execute's live field set.
        ///
        /// Walks View<VolumeField> off @p view.World, folds each component's authored knobs with its
        /// built field into a VolumeFieldInstance, and collects every live (non-null) field into
        /// m_VolumeFields for the volume pass to march — the lights model. When whether any live
        /// field exists changes between Executes, drives an internal Rebuild to insert or drop the
        /// pass at the frame boundary (reusing the imported output, so GetOutput() stays valid).
        /// @param view  The scene to resolve the volume fields from.
        void ResolveVolumeFields(const SceneView& view);

        /// @brief Fills the per-draw DrawData buffer (and, under GPU mode, the candidate buffer + groups) for this Execute.
        ///
        /// Computes the camera-frustum survivors, writes the current frame's DrawData region, and
        /// builds the geometry pass's submission plan (m_Internal->Plan). The geometry pass reads
        /// the plan at record time.
        /// @param view                The frame's scene view (broadphase already synced).
        /// @param viewConstantsIndex  This frame's view-constants ring region.
        /// @param halfResViewConstantsIndex The half-res layer's own view-constants region
        ///                            (equal to viewConstantsIndex when the layer claimed none).
        /// @param halfResViewReady    Whether the half-res layer's view region was claimed and
        ///                            written this frame; when not, gathered half-res draws fold
        ///                            back into the full-resolution plan.
        void PrepareDraws(const SceneView& view, u32 viewConstantsIndex,
                          u32 halfResViewConstantsIndex, bool halfResViewReady);

        /// @brief This frame's dynamic-resolution sub-rect and its UV mapping into the allocation.
        struct FrameScale
        {
            /// @brief round(m_Extent * scale), clamped to [1, m_Extent] — the rendered sub-rect.
            uvec2 ValidExtent;
            /// @brief ValidExtent / m_Extent — the fraction of the allocation this frame fills.
            vec2 RenderScaleUV;
            /// @brief (ValidExtent - 0.5) / m_Extent — the half-texel-inset clamp for a bilinear tap.
            vec2 MaxValidUV;
        };

        /// @brief Resolves this Execute's dynamic-resolution sub-rect from the view's render scale.
        ///
        /// A debug view, TAA, SSR, GPU hi-Z occlusion, or the Kawase bloom kernel each force full
        /// resolution (they do not carry the sub-rect sampling), so the scale applies only on the
        /// plain Final path.
        /// @param view  The frame's scene view (its RenderScale is the requested multiplier).
        /// @return The sub-rect extent and its UV mapping into the allocation.
        [[nodiscard]] FrameScale ResolveRenderScale(const SceneView& view) const;

        /// @brief Blends the candidate transforms between the last two Sim ticks by the frame alpha.
        ///
        /// Fills m_InterpolatedCandidates and points @p resolvedView.Visible at it only on a frame
        /// that interpolates (nonzero alpha and a scene with motion history); a static or
        /// tick-aligned frame leaves the broadphase's current-tick candidates in place.
        /// @param view          The frame's scene view (its Alpha and World drive the blend).
        /// @param resolvedView  The working view whose Visible span is repointed on interpolation.
        void ApplyTransformInterpolation(const SceneView& view, SceneView& resolvedView);

        /// @brief Resolves the scene's sky / point-field / volume-field components for this Execute.
        ///
        /// Resolves the one Sky component (recompiling the pass set at the frame boundary on a
        /// source-kind/tier/bake change), then the live point-field and volume-field sets, and
        /// forwards the resolved sky material to the sky-material pass. The lights model — the
        /// renderer reads the components off the scene rather than a consumer pushing them.
        /// @param resolvedView  The working view the resolved sky/field state is written into.
        void ResolveScenePasses(SceneView& resolvedView);

        /// @brief Builds this Execute's graph import bindings from the active targets and subsystems.
        ///
        /// The always-bound g-buffer / HDR / output / velocity / emissive imports plus the
        /// conditionally-declared battery imports (TAA, picking, shadows, bloom, auto-exposure,
        /// SSAO, refraction, SSR, hi-Z, and the GPU-cull indirect buffer) — appended only when the
        /// matching pass was wired, so the binding set matches the compiled graph's declared imports.
        /// @return The per-frame import bindings passed to CompiledGraph::Execute.
        [[nodiscard]] vector<RenderGraph::ImportBinding> BuildImportBindings();

        /// @brief Records this frame's view-projection, sub-rect mapping, and per-entity history.
        ///
        /// Captures the camera view-projection and sub-rect UVs for next frame's TAA reprojection,
        /// clears the TAA history-reset gate, advances the jitter frame index, and swaps this
        /// frame's worlds/palette bases into the previous-frame maps the velocity channel reads.
        /// @param viewProj  This frame's unjittered camera world→clip matrix.
        /// @param scale     This frame's resolved sub-rect and UV mapping.
        void RecordFrameHistory(const mat4& viewProj, const FrameScale& scale);

        /// @brief Vulkan context for all resource creation.
        Context& m_Context;
        /// @brief Asset manager for engine shader loading.
        AssetManager& m_Assets;
        /// @brief Pixel format of the owned output target.
        Format m_OutputFormat;
        /// @brief Allocated render extent — the high-water-mark every target is sized to.
        uvec2 m_Extent;
        /// @brief This frame's valid sub-rect extent (round(m_Extent * RenderScale)); GetValidExtent.
        uvec2 m_ValidExtent;
        /// @brief Previous frame's sub-rect UV mapping (validExtent/allocExtent), for TAA history.
        vec2 m_PreviousRenderScaleUV{1.0f};
        /// @brief Previous frame's clamped max valid UV ((validExtent-0.5)/allocExtent), for TAA history.
        vec2 m_PreviousMaxValidUV{1.0f};
        /// @brief Current topology and sizing knobs.
        SceneRendererSettings m_Settings;

        /// @brief Owned output image.
        Ref<Image> m_OutputImage;
        /// @brief View over m_OutputImage.
        Ref<ImageView> m_OutputView;

        /// @brief G-buffer targets (G0 albedo, G1 world-normal, G2 packed ORM, depth).
        ///
        /// Renderer-owned (sampled downstream, so not graph transients) and imported into
        /// the internal graph.
        Ref<Image> m_AlbedoImage;
        /// @brief View over m_AlbedoImage.
        Ref<ImageView> m_AlbedoView;
        /// @brief G1 world-normal image.
        Ref<Image> m_NormalImage;
        /// @brief View over m_NormalImage.
        Ref<ImageView> m_NormalView;
        /// @brief G2 packed ORM image.
        Ref<Image> m_OrmImage;
        /// @brief View over m_OrmImage.
        Ref<ImageView> m_OrmView;
        /// @brief Depth image.
        Ref<Image> m_DepthImage;
        /// @brief View over m_DepthImage.
        Ref<ImageView> m_DepthView;

        /// @brief Camera world->clip captured at the end of last Execute (this frame's pyramid pairs with it).
        ///
        /// Identity before the first Execute. Packed into the shared set-0 view-constants block's
        /// PrevViewProj unconditionally every frame, and passed to the GPU cull subsystem as the
        /// previous-frame matrix the occlusion test screen-bounds candidates with.
        mat4 m_PreviousViewProj{1.0f};

        /// @brief HDR target the deferred lighting pass writes (linear, unbounded range).
        ///
        /// Renderer-owned and imported like the g-buffer; tonemap maps it to the output format.
        /// Single-copy: one Execute resolves and completes before the next begins, so no
        /// cross-frame ring buffer is needed — the output is consumed in the frame it is written.
        Ref<Image> m_HdrImage;
        /// @brief View over m_HdrImage.
        Ref<ImageView> m_HdrView;

        /// @brief Bloom-mask target: the glow amplitude a forward material asks for apart from its radiance.
        ///
        /// Full extent, single-channel half-float, renderer-owned and imported like the g-buffer.
        /// The forward translucent pass clears it and a declaring material writes it as SV_Target1;
        /// the bloom pyramid's level-0 dispatch takes the larger of it and the luminance
        /// bright-pass and multiplies the filtered color by it, so an amplitude above 1 seeds the
        /// pyramid brighter than the surface's own radiance while that radiance stays inside the
        /// range the tone curve renders in full saturation.
        Ref<Image> m_BloomMaskImage;
        /// @brief View over m_BloomMaskImage.
        Ref<ImageView> m_BloomMaskView;

        /// @brief Per-object screen-space motion vector target — g-buffer channel G3.
        ///
        /// RG16Sfloat, full extent. The surface pass writes it as SV_Target3 alongside the
        /// other g-buffer channels every frame (no separate prepass), so it is always
        /// allocated; the TAA resolve and the MotionVectors debug blit read it. Created in
        /// CreateGBuffer and recreated on Resize/Configure with the rest of the g-buffer.
        Ref<Image> m_VelocityImage;
        /// @brief View over m_VelocityImage.
        Ref<ImageView> m_VelocityView;
        /// @brief Bindless slot for the velocity view; the resolve samples per-object motion through it.
        TextureHandle m_VelocityHandle;

        /// @brief HDR emissive target — g-buffer channel G4.
        ///
        /// B10G11R11Ufloat, full extent. The surface pass writes authored emission as SV_Target4
        /// alongside the other g-buffer channels every frame (no separate pass), so it is always
        /// allocated; the lighting pass samples it and adds it into the outgoing radiance. Created
        /// in CreateGBuffer and recreated on Resize/Configure with the rest of the g-buffer.
        Ref<Image> m_EmissiveImage;
        /// @brief View over m_EmissiveImage.
        Ref<ImageView> m_EmissiveView;
        /// @brief Bindless slot for the emissive view; the lighting pass samples G4 through it.
        TextureHandle m_EmissiveHandle;

        /// @brief LTC inverse-matrix lookup table for area-light shading (RGBA32F, LtcLut::Size²).
        Ref<Image> m_LtcMatImage;
        /// @brief View over m_LtcMatImage.
        Ref<ImageView> m_LtcMatView;
        /// @brief Bindless slot for the LTC matrix LUT.
        TextureHandle m_LtcMatHandle;
        /// @brief LTC magnitude/Fresnel lookup table for area-light shading (RGBA32F, LtcLut::Size²).
        Ref<Image> m_LtcMagImage;
        /// @brief View over m_LtcMagImage.
        Ref<ImageView> m_LtcMagView;
        /// @brief Bindless slot for the LTC magnitude LUT.
        TextureHandle m_LtcMagHandle;

        /// @brief The GPU occlusion-cull cluster + the hi-Z pyramid it tests against; created at Create.
        ///
        /// Owns the hi-Z reduce set/pipeline layouts + reduce pipeline, the pyramid image/views/sets,
        /// the cull compute cluster (candidate/indirect/count buffers, pipeline/layout/set), the
        /// cross-frame history-validity state, and the active cull mode. The SSR chain borrows its
        /// hi-Z reduce layouts, so it is constructed before the SSR chain; its pyramid is recreated
        /// from the g-buffer create/recreate tail (ResizeHiZ binds the fresh depth view).
        Unique<GpuCullSystem> m_GpuCull;

        /// @brief The screen-space-reflection chain — scene-color intermediate, mip chain, min-Z pyramid, and sweep.
        ///
        /// Its blur pipeline layout reserves the bloom down/up set layout and its min-Z reduce
        /// pipeline builds on the GPU cull subsystem's hi-Z reduce layout, so it is constructed after
        /// the GPU cull subsystem and the bloom subsystem exist; Recreate rebuilds the chain at the
        /// SsrResolution scale after the g-buffer depth and HDR targets.
        Unique<SsrChain> m_Ssr;

        /// @brief The depth-of-field battery — half-resolution layers, tile records, gather/fill
        ///        targets, and the composite that folds them over the lit HDR.
        ///
        /// Its tile and fill pipeline layouts reserve the bloom down/up set layout, so it is
        /// constructed after the bloom subsystem exists; Recreate rebuilds the chain after the
        /// g-buffer depth and HDR targets, whose views its prefilter set binds.
        Unique<DofChain> m_Dof;

        /// @brief The entity-id picking cluster + its request → stage → poll state machine; created at Create.
        ///
        /// Owns the R32Uint EntityId target + dedicated depth (allocated only while picking is on),
        /// the lazily-built static/skinned id pipelines, and the readback ring. The renderer wires
        /// its passes inline in Rebuild, reading this subsystem's pipeline pointers and graph ids.
        Unique<PickingSystem> m_Picking;

        /// @brief The pre-translucent refraction grab — scene-color/depth intermediates and the copy pipeline.
        Unique<RefractionGrab> m_Refraction;

        /// @brief The reduced-resolution translucent layer: targets, pipelines, and passes.
        Unique<HalfResTranslucency> m_HalfResTranslucent;

        /// @brief Bindless slots for the g-buffer/HDR views and the shared sampler.
        ///
        /// Registered once at Create; re-registered on Resize (old slots released through the
        /// per-frame retire window).
        TextureHandle m_AlbedoHandle;
        /// @brief Bindless slot for the world-normal view.
        TextureHandle m_NormalHandle;
        /// @brief Bindless slot for the ORM view.
        TextureHandle m_OrmHandle;
        /// @brief Bindless slot for the depth view.
        TextureHandle m_DepthHandle;
        /// @brief Bindless slot for the HDR view.
        TextureHandle m_HdrHandle;
        /// @brief Bindless slot for the bloom-mask view; the bloom down-sweep samples it through the registry.
        TextureHandle m_BloomMaskHandle;
        /// @brief Bindless slot of the linear clamp sampler the fullscreen passes read the g-buffer
        /// and HDR target through, shared out of the registry across every SceneRenderer.
        SamplerHandle m_SamplerHandle;

        /// @brief Engine-owned lighting pipeline writing the HDR format.
        ///
        /// Built once at Create from the core pack's shaders. The pass set Mode references the
        /// pipelines it needs; the rest stay built but unused.
        Ref<class GraphicsPipeline> m_LightingPipeline;
        /// @brief Layout for m_LightingPipeline.
        Ref<class PipelineLayout> m_LightingLayout;

        /// @brief SSAO-enabled lighting variant; selected when Settings.AO is on.
        ///
        /// A separate fragment shader compiled with the AO fold. SSAO is a compile-time pipeline
        /// variant, not a per-frame branch.
        Ref<class GraphicsPipeline> m_SsaoLightingPipeline;
        /// @brief Layout for the SSAO lighting variant.
        Ref<class PipelineLayout> m_SsaoLightingLayout;

        /// @brief Cascade-debug lighting variant (DebugView::Cascades).
        ///
        /// Tint fragment shader over the plain lighting layout (set 1 + non-SSAO push block),
        /// writing the output format directly. Reuses m_LightingLayout.
        Ref<class GraphicsPipeline> m_CascadeDebugPipeline;

        /// @brief Fullscreen skybox pipeline (radiance cube over the lit HDR), writing HdrFormat.
        Ref<class GraphicsPipeline> m_SkyboxPipeline;
        /// @brief Layout for m_SkyboxPipeline: the IBL set (set 1) + the skybox push block.
        Ref<class PipelineLayout> m_SkyboxLayout;

        /// @brief Fullscreen procedural-atmosphere sky pipeline (LUTs over the lit HDR), writing HdrFormat.
        Ref<class GraphicsPipeline> m_SkyPipeline;
        /// @brief Layout for m_SkyPipeline: the atmosphere set (set 1) + the sky push block.
        Ref<class PipelineLayout> m_SkyLayout;

        /// @brief SSAO fullscreen pipeline writing the R8 AO target.
        Ref<class GraphicsPipeline> m_SsaoPipeline;
        /// @brief Layout for the SSAO pipeline.
        Ref<class PipelineLayout> m_SsaoLayout;

        /// @brief The fullscreen debug-blit pipelines for the non-Final DebugView arms.
        ///
        /// One aggregate owning the g-buffer/channel/target blit pipeline+layout pairs
        /// (albedo, normal, depth, packed-ORM, SSAO, motion, directional shadow, CoC) each arm's
        /// terminal blit selects. Built once at Create beside the core pipelines. Held behind
        /// an opaque pointer so this header stays free of the pipeline aggregate's definition.
        Unique<DebugBlitPipelines> m_DebugBlits;

        /// @brief The set-1 shadow descriptor system + punctual atlas + constants rings; created at Create.
        ///
        /// Owns the comparison sampler, the set-1 layout/set, the debug-blit layout/set/sampler,
        /// the dummy and punctual atlases, and both constants rings. The lighting layout reserves
        /// its set layout, so it exists before the pipelines. The directional cascade atlas is not
        /// owned here — ShadowScenePass owns it and Rebuild binds its view (or the dummy) into the
        /// set through RebuildSets.
        Unique<ShadowSystem> m_Shadows;

        /// @brief The compute mip-pyramid bloom battery — pyramid, pipelines, sets, and the sweep.
        ///
        /// Its down/up set layout is reserved by the SSR blur pipeline layout, so it is constructed
        /// before CreatePipelines; Resize rebuilds the extent-sized pyramid after the HDR target.
        Unique<BloomPyramid> m_Bloom;

        /// @brief The auto-exposure metering battery — histogram pipeline, ring, and adaptation state.
        Unique<AutoExposureMeter> m_AutoExposure;

        /// @brief The TAA resolve battery — resolve/copy pipelines, lit/history targets, reset gate.
        Unique<TaaResolve> m_Taa;

        /// @brief Number of frames-in-flight the renderer-owned rings are sized for.
        ///
        /// Seeded at construction (before the ring allocations below) and read by the draw-data,
        /// skinning-palette, and cull rings.
        u32 m_FramesInFlight = 0;

        /// @brief Core tonemap PostProcess material, loaded once at Create.
        ///
        /// The Final chain's terminal PostProcessScenePass drives it (HDR target as the
        /// runtime-bound input; Exposure written per Execute into its param block).
        AssetHandle<MaterialInstance> m_TonemapMaterial;

        /// @brief Renderer-owned pass units; rebuilt per Settings.Mode on every Rebuild.
        ///
        /// The geometry pass is always first; Mode selects the tail.
        vector<Unique<ScenePass>> m_Passes;

        /// @brief The point-field pass, held outside m_Passes and declared at the HDR tail anchor.
        ///
        /// The fields accumulate into the final HDR between the SSR composite and the bloom
        /// sweep — a position only the tail anchor occupies, so the pass cannot ride the list.
        /// Null unless the Final arm is built while a live HdrTail-placed field exists
        /// (m_PointFieldActive).
        Unique<ScenePass> m_PointFieldPass;

        /// @brief The depth-of-field composite, held outside m_Passes and declared at the HDR tail
        ///        anchor between the point fields and the bloom sweep.
        ///
        /// Null unless the chain is fully wired (the Final arm with Settings.DepthOfField); the
        /// CoC debug arm declares only the chain's first two compute stages, never this.
        Unique<ScenePass> m_DofCompositePass;

        /// @brief The scene-color point-field pass, for fields placed in the lit scene color.
        ///
        /// Rides m_Passes (it writes the in-list io.Hdr lit target), inserted ahead of the
        /// refraction copy and the translucent pass so translucents blend over the fields and the
        /// scene-color grab includes them. A non-owning observer for the stats/force-direct
        /// accessors; null unless the Final arm is built while a live SceneColor-placed field
        /// exists (m_ScenePointFieldActive).
        ScenePass* m_ScenePointFieldPass = nullptr;

        /// @brief BVH broadphase over resident draw candidates.
        ///
        /// Synced once at the top of Execute; its candidate span is pointed at by
        /// SceneView::Visible and its tree is queried by the g-buffer and shadow passes.
        /// A static scene does not rebuild the tree.
        SceneBroadphase m_Broadphase;

        /// @brief Per-frame copy of the broadphase candidates with interpolated world transforms.
        ///
        /// Filled only on a frame that interpolates (nonzero alpha and a scene with motion history):
        /// each candidate's World is re-blended between the last two Sim-tick snapshots and its
        /// WorldBounds recomputed, and SceneView::Visible points here instead of at the broadphase's
        /// current-tick candidates. Empty (and unused) on a static or tick-aligned frame.
        vector<VisibleMesh> m_InterpolatedCandidates;

        /// @brief Per-submesh frustum-survivor count from the last Execute.
        ///
        /// Set by PrepareDraws each Execute: the number of per-submesh candidates the camera
        /// frustum kept (a materialless or not-yet-resident survivor still counts). The middle
        /// funnel stage; the upload count under CullMode::GPU. Zero before the first Execute.
        u32 m_FrustumSurvivedCount = 0;

        /// @brief Per-submesh drawn count from the last Execute (equals m_FrustumSurvivedCount).
        ///
        /// The terminal funnel stage: every frustum survivor is a draw under CullMode::CPU.
        /// Zero before the first Execute.
        u32 m_LastDrawnCount = 0;

        /// @brief The last Execute's draw-slot / palette budget accounting.
        DrawBudgetStats m_DrawBudgetStats;
        /// @brief Set once the slot-budget clamp has logged, so the WARN fires only once.
        bool m_DrawSlotBudgetWarned = false;
        /// @brief Set once the palette-budget clamp has logged, so the WARN fires only once.
        bool m_PaletteBudgetWarned = false;

        /// @brief Set once the cascade-budget message has logged, so the WARN fires only once.
        bool m_CascadeBudgetWarned = false;

        /// @brief Warns once per renderer, per budget, when the last gather clamped.
        ///
        /// Reads m_DrawBudgetStats after the gather phases have run. An overflowed scene is a
        /// steady state rather than a transient, so each budget's message fires once for the life
        /// of the renderer and the per-frame signal is the stats block itself.
        void ReportDrawBudgetDrops();

        /// @brief Warns once per renderer when a shadow-casting directional was denied a cascade set.
        ///
        /// A scene lighting from more near-parallel sources than the atlas carries sets is a
        /// steady state, so the message fires once for the life of the renderer; the per-frame
        /// signal is PackedSceneLights::DeniedDirectionalCount and the light's own denied flag.
        /// @param denied Shadow-casting directionals the last pack could not seat.
        void ReportDeniedCascades(u32 denied);

        /// @brief Allocates the mode-independent per-draw buffers + their descriptor sets.
        ///
        /// The per-draw DrawData SSBO, the skinning palette, and the identity candidate-id buffer —
        /// used by both cull modes (the buffer-indexed surface draw) — sized to
        /// MaxCullCandidates × frames-in-flight. The GPU-cull candidate/indirect/count buffers and
        /// the cull compute pipeline live on m_GpuCull. Called once at Create.
        void CreateCullResources();

        /// @brief Per-draw DrawData SSBO (set used by the surface pipeline's set 1, binding 0).
        ///
        /// Host-visible, ring-buffered for frames-in-flight (MaxCullCandidates records per region);
        /// the surface vertex stage reads its record by the candidate id folded with the pushed
        /// FrameBase. Drives both cull modes' buffer-indexed draw.
        Ref<Buffer> m_DrawDataBuffer;
        /// @brief Set 1 for the surface pipeline: binding 0 the DrawData SSBO.
        Ref<DescriptorSetLayout> m_DrawDataSetLayout;
        /// @brief Descriptor set bound at set 1 for every surface draw.
        Ref<DescriptorSet> m_DrawDataSet;

        /// @brief Identity candidate-id buffer bound to vertex binding 1 (instance rate).
        ///
        /// Element k holds k, so a draw's firstInstance = candidateId fetches candidateId as the
        /// instance attribute (the per-draw DrawData index). Created once; shared by both cull
        /// modes' draws. MaxCullCandidates elements.
        Ref<Buffer> m_CandidateIdBuffer;

        /// @brief Maximum skinning matrices uploaded per frame across all skinned instances.
        static constexpr u32 MaxSkinningMatricesPerFrame = 8192;

        /// @brief Per-instance skinning palette (mat4 per bone), bound at set 2 for skinned draws.
        ///
        /// Host-visible, ring-buffered for frames-in-flight (MaxSkinningMatricesPerFrame matrices
        /// per region). Each skinned instance's bones are appended contiguously and its DrawData
        /// PaletteBase is the absolute index of its first bone in this buffer.
        Ref<Buffer> m_PaletteBuffer;
        /// @brief Set 2 for the skinned surface pipeline / set 1 for the skinned shadow pipeline: the palette SSBO.
        Ref<DescriptorSetLayout> m_PaletteSetLayout;
        /// @brief Descriptor set holding the palette buffer, bound for skinned draws.
        Ref<DescriptorSet> m_PaletteSet;
        /// @brief This frame's PaletteBase per skinned entity (packed Entity → base), read by the shadow passes.
        unordered_map<u64, u32> m_PaletteBaseByEntity;
        /// @brief Previous frame's PaletteBase per skinned entity; surface_skinned.vert skins the
        ///        previous position through it for velocity. Swapped from m_PaletteBaseByEntity each frame.
        unordered_map<u64, u32> m_PreviousPaletteBaseByEntity;

        /// @brief Reused per-frame frustum-survivor candidate ids (broadphase Cull scratch).
        ///
        /// Filled by PrepareDraws (the frustum descent) and iterated to lay out the draw slots for
        /// both cull modes; the GPU cull's device-side buffers live on m_GpuCull.
        vector<u32> m_CullScratch;

        /// @brief Imported resource ids re-declared on every Rebuild.
        ///
        /// Bound to their concrete views per Execute and threaded to pass units through PassIO.
        ResourceId m_AlbedoId;
        /// @brief Imported id for the world-normal target.
        ResourceId m_NormalId;
        /// @brief Imported id for the packed ORM target.
        ResourceId m_OrmId;
        /// @brief Imported id for the depth target.
        ResourceId m_DepthId;
        /// @brief Imported id for the HDR target.
        ResourceId m_HdrId;
        /// @brief Imported id for the lighting target under TAA (the resolve's current input).
        ResourceId m_LitId;
        /// @brief Imported id for the persisted TAA history target.
        ResourceId m_TaaHistoryId;
        /// @brief Imported id for the velocity g-buffer channel (G3), written every frame.
        ResourceId m_VelocityId;
        /// @brief Imported id for the emissive g-buffer channel (G4), written every frame.
        ResourceId m_EmissiveId;
        /// @brief Per-mip subresource handle for the bloom pyramid the down/up sweep reads and writes.
        MipChainId m_BloomChainId;
        /// @brief Imported id for the bloom composite result.
        ResourceId m_BloomResultId;
        /// @brief Imported id for the bloom-mask target the translucent pass writes and the down-sweep reads.
        ///
        /// Invalid when bloom is off, which is what takes the mask attachment off the translucent
        /// pass: nothing would read it.
        ResourceId m_BloomMaskId;
        /// @brief Imported buffer id for the auto-exposure histogram buffer.
        ResourceId m_AutoExposureId;
        /// @brief Imported id for the directional shadow atlas.
        ResourceId m_ShadowId;
        /// @brief Imported id for the SSAO target.
        ResourceId m_SsaoId;
        /// @brief Imported id for the SSR lit scene-color intermediate.
        ResourceId m_SsrSceneId;
        /// @brief Imported id for the depth-of-field near-field prefiltered layer.
        ResourceId m_DofNearId;
        /// @brief Imported id for the depth-of-field far-field prefiltered layer.
        ResourceId m_DofFarId;
        /// @brief Imported id for the depth-of-field signed-radius/view-depth target.
        ResourceId m_DofCocId;
        /// @brief Imported id for the depth-of-field tile records.
        ResourceId m_DofTileId;
        /// @brief Imported id for the near-layer ring-gather destination.
        ResourceId m_DofNearBlurId;
        /// @brief Imported id for the far-layer ring-gather destination.
        ResourceId m_DofFarBlurId;
        /// @brief Imported id for the near-layer fill destination.
        ResourceId m_DofNearFillId;
        /// @brief Imported id for the far-layer fill destination.
        ResourceId m_DofFarFillId;
        /// @brief Imported id for the depth-of-field lit scene-color intermediate.
        ResourceId m_DofSceneId;
        /// @brief Imported id for the refraction scene-color intermediate.
        ResourceId m_RefractionSceneId;
        /// @brief Imported id for the refraction scene-depth intermediate.
        ResourceId m_RefractionDepthId;
        /// @brief One graph id per scene-color chain level, base first; empty when refraction is off.
        vector<ResourceId> m_RefractionMipIds;
        /// @brief Imported id for the half-res translucent layer color target.
        ResourceId m_HalfResLayerId;
        /// @brief Imported id for the half-res translucent reduced depth target.
        ResourceId m_HalfResDepthReducedId;
        /// @brief Per-mip subresource handle for the SSR reflection pyramid (trace + blur).
        MipChainId m_SsrReflectionChainId;
        /// @brief Per-mip subresource handle for the SSR min-Z pyramid (reduction + trace).
        MipChainId m_SsrHiZChainId;
        /// @brief Imported id for the final output target.
        ResourceId m_OutputId;

        /// @brief Which passes the last Rebuild wired, decided from the settings plus the sky.
        ///
        /// Held behind a forward declaration because the struct lives in a src-private header this
        /// installed one cannot include. Rebuild replaces it wholesale; Execute and
        /// BuildImportBindings read it back to bind exactly the imports the wired passes declared.
        Unique<FrameTopology> m_Topology;

        /// @brief Monotonic frame counter driving the Halton jitter sequence.
        ///
        /// Incremented every Execute (independent of the TAA toggle so toggling on does not
        /// snap the sequence). Folds into TaaJitterSampleCount.
        u64 m_FrameIndex = 0;

        /// @brief Previous frame's world matrix per entity, keyed by a packed Entity id.
        ///
        /// The surface pass writes velocity from each drawn object's prior transform; PrepareDraws
        /// looks it up here and writes it into the per-draw record (DrawData.PrevWorld). An entity
        /// absent (first seen) reprojects with zero object motion. Maintained every frame (velocity
        /// is always written); swapped from m_CurrentWorlds at the end of each Execute.
        unordered_map<u64, mat4> m_PreviousWorlds;
        /// @brief This frame's world matrix per entity; swapped into m_PreviousWorlds after Execute.
        unordered_map<u64, mat4> m_CurrentWorlds;

        /// @brief Non-owning pointer to the wired ShadowScenePass, or null when shadows are compiled out.
        ///
        /// The renderer reads its produced atlas view to thread into PassIO and to bind the shadow
        /// import per Execute. The pass outlives this pointer (m_Passes is cleared and rebuilt together).
        ShadowScenePass* m_ShadowPass = nullptr;

        /// @brief Imported id for the punctual shadow atlas.
        ResourceId m_PunctualShadowId;
        /// @brief Non-owning pointer to the wired punctual shadow pass.
        PunctualShadowScenePass* m_PunctualShadowPass = nullptr;

        /// @brief Non-owning pointer into m_Passes to the SsaoScenePass; null when AO is off.
        class SsaoScenePass* m_SsaoPass = nullptr;

        /// @brief Non-owning pointer into m_Passes to the SkyMaterialScenePass; null when the sky is not a material.
        ///
        /// Execute forwards the resolved SceneView::SkyMaterial to it before the graph runs.
        class SkyMaterialScenePass* m_SkyMaterialPass = nullptr;

        /// @brief The sky-resolve state machine and the three sky radiance-cube helpers; created at Create.
        ///
        /// Owns the image-based-lighting maps (set 2 for the lighting pass), the procedural-atmosphere
        /// LUTs (set 1 for the sky pass), and the baked-sky cube, plus the whole resolve state machine
        /// (resolved source-kind/tier/bake-mode, the once-per-change dirty gates, the projected
        /// skylight SH). Created before CreatePipelines so the lighting and sky layouts reserve its
        /// consumer set layouts; Rebuild reaches the sets/layouts and the resolved kind/tier through
        /// its getters.
        Unique<SkyResolver> m_SkyResolver;

        /// @brief Immediate-mode debug-draw accumulator flushed by the DebugDrawScenePass.
        ///
        /// Mutable so GetDebugDraw() (a const accessor) hands out a writable reference: the
        /// renderer's constness is its own identity, not the per-frame accumulator. Cleared at
        /// the start of every Execute.
        mutable DebugDraw m_DebugDraw;

        /// @brief This Execute's live HdrTail-placed point fields, borrowed from the scene's
        /// PointField components.
        ///
        /// Refilled every Execute by ResolvePointFields walking View<PointField>; the point-field
        /// pass reads it by pointer and draws each field. A borrow only — the scene's components own
        /// the fields' lifetimes, so a dropped field is simply absent next Execute (no teardown
        /// ordering). Empty when no component carries a live field.
        vector<const PointField*> m_PointFields;

        /// @brief This Execute's live SceneColor-placed point fields; m_ScenePointFieldPass's set.
        vector<const PointField*> m_ScenePointFields;

        /// @brief Persisted point-field force-direct hook, reapplied when the passes are rebuilt.
        bool m_PointFieldForceDirect = false;

        /// @brief Whether the current pass set carries the tail point-field pass; gates the
        /// internal Rebuild.
        ///
        /// True once ResolvePointFields has seen a live field and wired the pass. Compared against
        /// each Execute's presence so the pass inserts on the first live field and drops when the
        /// last one goes, recompiling at the frame boundary (reusing the imported output).
        bool m_PointFieldActive = false;

        /// @brief Whether the current pass set carries the scene-color point-field pass.
        bool m_ScenePointFieldActive = false;

        /// @brief This Execute's live volume fields, resolved from the scene's VolumeField components.
        ///
        /// Refilled every Execute by ResolveVolumeFields walking View<VolumeField>; the volume pass
        /// reads it by pointer and marches each field. A borrow of the built fields (the scene's
        /// components own their lifetimes), with the authored knobs folded in per instance. Empty
        /// when no component carries a live field.
        vector<VolumeFieldInstance> m_VolumeFields;

        /// @brief Whether the current pass set carries the volume pass; gates the internal Rebuild.
        ///
        /// True once ResolveVolumeFields has seen a live field and wired the pass. Compared against
        /// each Execute's presence so the pass inserts on the first live field and drops when the last
        /// one goes, recompiling at the frame boundary (reusing the imported output).
        bool m_VolumeFieldActive = false;

        /// @brief True while the translucent gather routes draws into the half-res layer.
        ///
        /// Content-driven, the volume-field model: set the first Execute whose gather routed an
        /// opted-in material's draw into the half plan (that frame's draws fold back into the
        /// full-res plan and the layer is wired for the next), cleared when the last one goes;
        /// each edge recreates the layer targets and rebuilds the pass set.
        bool m_HalfResTranslucentActive = false;

        /// @brief Opaque compiled graph; replayed every Execute.
        ///
        /// Held behind an opaque pointer so this header stays free of the full CompiledGraph type.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
