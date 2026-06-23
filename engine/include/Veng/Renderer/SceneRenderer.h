#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/HiZHistory.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/ShadowCascades.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneBroadphase.h>
#include <Veng/Scene/Visibility.h>

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
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ScenePass;
    class ShadowScenePass;
    class PunctualShadowScenePass;
    class Image;
    class Sampler;
    class Buffer;
    class DescriptorSet;
    class DescriptorSetLayout;

    /// @brief Maximum number of simultaneously shadowed point/spot lights.
    ///
    /// The first N shadow-casting punctual lights (by per-frame selection) receive a
    /// shadow map; the rest are lit without shadows. A point light costs six cube-face
    /// redraws of its caster set, so N bounds the punctual shadow atlas and the lighting
    /// loop's sample set at 6N depth tiles.
    inline constexpr u32 MaxShadowedPunctual = 4;

    /// @brief Per-shadowed-light GPU record uploaded to set 1 binding 3.
    ///
    /// glm-only — no backend types — so it lives in a public header. Its layout is
    /// std140/std430-identical to the shader's PunctualShadowRecord, so the same struct
    /// serves a uniform or SSBO binding.
    struct PunctualShadowRecord
    {
        /// @brief World → light-clip transforms with atlas tile-remap baked in (384 bytes).
        ///
        /// [0] for a spot's single perspective view; [0..5] for a point's six cube faces
        /// in CubeFace order. A lit fragment projected by ViewProj[f] lands in this
        /// light's atlas tile f, so the lighting pass samples the correct tile.
        mat4 ViewProj[CubeFaceCount];

        /// @brief World position (xyz) and falloff range (w) (16 bytes).
        ///
        /// xyz is the light's world position for cube-face selection and depth
        /// linearization; w is the range the depth pass projects to.
        vec4 PositionRange;

        /// @brief Type (x), near (y), far (z), depth bias (w) (16 bytes).
        ///
        /// x encodes the light type: 1 = point, 2 = spot, 0 = unused/zeroed slot.
        vec4 Params;
    };

    /// @brief Selects which result the renderer produces, re-wiring the pass set.
    ///
    /// Final is the full deferred chain (g-buffer → lighting → tonemap). All other
    /// values terminate the chain after the g-buffer with a single fullscreen debug blit
    /// of one channel or target. A change here is a topology change driven through
    /// Configure → recompile.
    ///
    /// Roughness/Metallic/Occlusion read the packed G2 ORM channels (R=occlusion,
    /// G=roughness, B=metallic). AO reads the SSAO target and Shadows the directional
    /// shadow map; the producing pass is force-wired in those modes regardless of the
    /// Settings.AO / Settings.Shadows toggle. Cascades tints each fragment by the
    /// cascade its view-space depth selects (0 red, 1 green, 2 blue, 3 yellow),
    /// force-wiring the shadow pass so cascade constants are present. PunctualShadows
    /// blits the punctual shadow atlas (raw depth), force-wiring the punctual shadow pass.
    /// Bloom runs the lighting pass and the bloom pyramid sweep, then blits pyramid mip 0
    /// after the up-sweep — the accumulated bloom contribution before composite —
    /// force-wiring the bloom pass regardless of the Settings.Bloom toggle. MotionVectors blits
    /// the per-object velocity target colorized as an optical-flow field, force-wiring the TAA
    /// velocity prepass regardless of the Settings.TAA toggle.
    enum class DebugView : u8
    {
        /// @brief Full deferred pipeline output.
        Final,
        /// @brief G0 base color channel.
        Albedo,
        /// @brief G1 world-space normal channel.
        Normal,
        /// @brief Depth buffer visualized as a linear grey scale.
        Depth,
        /// @brief G2 roughness channel (green).
        Roughness,
        /// @brief G2 metallic channel (blue).
        Metallic,
        /// @brief G2 ambient occlusion channel (red).
        Occlusion,
        /// @brief SSAO target (force-wires the SSAO pass).
        AO,
        /// @brief Directional shadow atlas raw depth (force-wires the shadow pass).
        Shadows,
        /// @brief Per-fragment cascade tint (force-wires the shadow pass).
        Cascades,
        /// @brief Punctual shadow atlas raw depth (force-wires the punctual shadow pass).
        PunctualShadows,
        /// @brief Accumulated bloom pyramid mip 0 after the up-sweep (force-wires the bloom pass).
        Bloom,
        /// @brief Per-object velocity as an optical-flow field (force-wires the TAA velocity prepass).
        MotionVectors,
    };

    /// @brief Selects the bloom pyramid's down/up filter kernel.
    ///
    /// The kernel choice changes the per-level compute shader, so it is a topology knob
    /// (a Configure recompile), like the Bloom toggle. Cod is the reference filter the
    /// golden is blessed against; Kawase is the bandwidth-optimized alternative.
    enum class BloomKernel : u8
    {
        /// @brief Call of Duty / Jimenez 13-tap downsample + 3×3 tent upsample dual filter.
        Cod,
        /// @brief Dual Kawase 5-tap downsample + 8-tap upsample bilinear filter (Bjørge),
        ///        designed for the bandwidth-bound tile-based GPUs veng primarily targets.
        Kawase,
    };

    /// @brief Topology and sizing knobs for SceneRenderer.
    ///
    /// A change to any field here is a Configure → recompile. Knobs that turn a pass
    /// on/off or re-wire the pass set live here; per-frame values belong on SceneView.
    struct SceneRendererSettings
    {
        /// @brief Selects whether culling and draw submission run CPU-side or GPU-driven.
        ///
        /// CPU is the BVH frustum descent plus direct per-submesh DrawIndexed calls — the
        /// default and the fallback where multiDrawIndirect / drawIndirectFirstInstance is
        /// unavailable. GPU keeps the same BVH frustum descent (the upload source) but runs
        /// the hi-Z occlusion test in a compute pass that writes each indirect command's
        /// instanceCount, then issues the survivors through vkCmdDrawIndexedIndirect. Both
        /// modes drive the same buffer-indexed surface shader — they differ only in
        /// submission and in who writes instanceCount. Nested to avoid the name collision
        /// with Renderer::CullMode (the rasterizer face-cull mode).
        enum class CullMode : u8
        {
            /// @brief BVH frustum descent + direct per-submesh DrawIndexed.
            CPU,
            /// @brief BVH frustum descent + GPU hi-Z occlusion + vkCmdDrawIndexedIndirect.
            GPU,
        };

        /// @brief Selects which result the renderer produces; re-wires the pass set on change.
        DebugView Mode = DebugView::Final;

        /// @brief Whether the compute mip-pyramid bloom runs ahead of tonemap.
        ///
        /// A topology change: it inserts/removes the bloom down/up/composite compute sweep.
        /// BloomThreshold, BloomIntensity, and BloomRadius are per-frame values on SceneView
        /// and do not trigger a recompile.
        bool Bloom = true;

        /// @brief Selects the bloom pyramid's down/up filter kernel.
        ///
        /// A topology change: it selects the compiled down/up compute pipeline. The Cod
        /// default is the golden's kernel; Kawase is the bandwidth-optimized alternative.
        BloomKernel Kernel = BloomKernel::Cod;

        /// @brief Whether temporal anti-aliasing resolves the lit image.
        ///
        /// A topology change: it jitters the projection, inserts the TAA resolve and
        /// history-copy passes between lighting and tonemap, and routes lighting into a
        /// separate target the resolve reads. Off by default. Motion vectors are
        /// camera-only (reconstructed from depth), so a moving camera over static
        /// geometry resolves exactly while dynamic objects ghost.
        bool TAA = false;

        /// @brief Whether the directional light casts a shadow.
        ///
        /// A topology change: it inserts/removes the depth-only ShadowScenePass and the
        /// lighting pass's shadow sample. When off, the lighting pass reads full
        /// visibility for the directional term.
        bool Shadows = true;

        /// @brief Whether the bounded set of point/spot lights cast shadows.
        ///
        /// A topology change: it inserts/removes the depth-only PunctualShadowScenePass
        /// and the lighting pass's per-light sample. When off, the per-light selection
        /// writes slot -1 to every light and the lighting pass reads full visibility for
        /// every punctual term. MaxShadowedPunctual caps the number of shadowed lights
        /// when enabled.
        bool PunctualShadows = true;

        /// @brief Per-cascade shadow tile edge length in texels.
        ///
        /// Changing this recreates the shadow atlas and recompiles. A default 4-cascade
        /// atlas at 1024 is 2048² — the same footprint as a single 2048 map. Values
        /// above GetMaxShadowResolution() are clamped before any atlas is sized.
        u32 ShadowResolution = 1024;

        /// @brief Per-tile edge length in texels of the punctual shadow atlas.
        ///
        /// A spot uses one tile; a point uses six cube-face tiles. Changing this recreates
        /// the punctual atlas and recompiles. The atlas is MaxShadowedPunctual·CubeFaceCount
        /// tiles of this resolution. Values above GetMaxPunctualShadowResolution() are
        /// clamped.
        u32 PunctualShadowResolution = 1024;

        /// @brief Number of shadow cascades the directional light's frustum is split into.
        ///
        /// Clamped to [1, MaxCascades]. Sizing: it sizes the atlas tile grid
        /// (min(Count,2)×ceil(Count/2)), so it recreates the atlas and recompiles.
        u32 CascadeCount = MaxCascades;

        /// @brief PSSM split blend factor (0 = uniform splits, 1 = logarithmic).
        ///
        /// Recompile-safe: it changes the cascade fit, not the atlas size or topology.
        f32 CascadeSplitLambda = 0.85f;

        /// @brief Whether the screen-space ambient occlusion pass runs.
        ///
        /// A topology change: it inserts/removes the fullscreen SsaoScenePass and
        /// selects the lighting pipeline variant that folds the AO target into the
        /// ambient term. SSAO modulates the ambient/indirect term only; kernel constants
        /// (radius/intensity/bias) are fixed in the SSAO shader.
        bool AO = true;

        /// @brief Whether scene passes cull by frustum.
        ///
        /// The g-buffer pass tests each mesh's world bound against the camera frustum;
        /// the shadow pass tests it against each cascade's light frustum. When off both
        /// record every resident mesh. A toggle drives a recompile (the same passes
        /// record fewer draws), so it still invalidates GetOutput() like any Configure.
        bool FrustumCull = true;

        /// @brief Selects CPU direct draws or the GPU-driven occlusion-cull → indirect-draw path.
        ///
        /// GPU is honored only where Context::IsGpuDrivenCullingSupported() is true; otherwise
        /// the renderer falls back to the CPU path. A change recompiles (the GPU path is a
        /// different pass topology).
        CullMode Cull = CullMode::CPU;

        /// @brief Whether the GPU path runs the hi-Z occlusion test (GPU mode only).
        ///
        /// When off, the GPU path issues every camera-frustum survivor (frustum-only). When on,
        /// the cull compute pass drops the provably-occluded against the previous-frame pyramid.
        /// Ignored under CullMode::CPU. A history-invalid frame is frustum-only regardless.
        bool Occlusion = true;
    };

    /// @brief Construction parameters for SceneRenderer.
    struct SceneRendererInfo
    {
        /// @brief The Vulkan context for resource creation.
        Context& Context;
        /// @brief Asset manager used to load engine shaders (lighting pass, fullscreen blit).
        ///
        /// Must outlive the renderer.
        AssetManager& Assets;
        /// @brief Pixel format of the owned output target.
        Format OutputFormat = Format::Undefined;
        /// @brief Initial render extent.
        uvec2 Extent = {};
        /// @brief Initial topology and sizing knobs.
        SceneRendererSettings Settings;
    };

    /// @brief Per-frame input for SceneRenderer::Execute.
    ///
    /// Not owned by the renderer; World and Camera are borrowed references. The renderer
    /// overwrites the output fields (LightCount, CascadeViewProj, etc.) on every Execute
    /// — a caller's values in those fields are ignored.
    ///
    /// The renderer reads the scene's lights itself: on every Execute it walks
    /// View<Transform, Light> up to MaxLights, packs each into the ring-buffered light
    /// buffer, and the lighting pass loops over the live count. A scene with no Light
    /// renders flat-ambient.
    struct SceneView
    {
        /// @brief The scene to render.
        const Scene& World;
        /// @brief The viewpoint to render from.
        const CameraView& Camera;
        /// @brief Frame delta time in seconds.
        f32 Delta = 0.0f;

        /// @brief Live light count this frame; set by the renderer on every Execute.
        ///
        /// The number of (Transform, Light) entities packed, capped at MaxLights. The
        /// lighting pass loops [0, LightCount). A caller's value is overwritten.
        u32 LightCount = 0;

        /// @brief Maximum number of lights the renderer packs per frame.
        static constexpr u32 MaxLights = BindlessRegistry::MaxLights;

        /// @brief Exposure scale applied before the tone curve; written into the tonemap material's param block each Execute.
        ///
        /// Read fresh every Execute, so tuning it never triggers a recompile.
        f32 Exposure = 1.0f;

        /// @brief Bloom bright-pass luminance knee; pushed to the downsample compute each Execute.
        ///
        /// The soft-knee threshold the HDR → mip 0 downsample applies. Tuning this rides the
        /// compute push, so it does not trigger a recompile. Ignored when bloom is inactive.
        f32 BloomThreshold = 1.0f;
        /// @brief Bloom composite mix intensity; pushed to the composite compute each Execute.
        ///
        /// Scales the accumulated bloom added back into the HDR. Tuning this rides the compute
        /// push, so it does not trigger a recompile. Ignored when bloom is inactive.
        f32 BloomIntensity = 1.0f;
        /// @brief Bloom upsample spread; pushed to the upsample compute each Execute.
        ///
        /// Scales each tent up-step's contribution as it accumulates back up the pyramid, so a
        /// larger value spreads the glow wider. Rides the compute push (no recompile). Ignored
        /// when bloom is inactive.
        f32 BloomRadius = 1.0f;

        /// @brief RAW (non-tile-remapped) per-cascade world → light-clip transforms this frame.
        ///
        /// Computed by the renderer on every Execute from the first directional light (identity
        /// when there is none). The shadow pass renders cascade k with CascadeViewProj[k] pushed
        /// and the viewport placing it in its atlas tile. Only [0, CascadeCount) are valid. The
        /// lighting pass reads the tile-remapped matrices from the set-1 ShadowConstants buffer.
        /// A caller's values are overwritten.
        std::array<mat4, MaxCascades> CascadeViewProj{};
        /// @brief Number of valid entries in CascadeViewProj; set by the renderer each Execute.
        u32 CascadeCount = 0;

        /// @brief Shadowed punctual lights selected this frame (the first MaxShadowedPunctual shadow-casting lights).
        ///
        /// The punctual shadow pass renders each record's views into the atlas; the lighting pass
        /// samples Records[slot]. PunctualShadowCount records are valid. A caller's values are
        /// overwritten by the renderer each Execute.
        std::array<PunctualShadowRecord, MaxShadowedPunctual> PunctualShadows{};
        /// @brief Number of valid entries in PunctualShadows.
        u32 PunctualShadowCount = 0;

        /// @brief RAW (non-tile-remapped) per-record/per-face world → light-clip transforms this frame.
        ///
        /// Parallel to PunctualShadows; computed by the renderer on every Execute. A spot fills
        /// [slot][0]; a point fills [slot][0..5]. The punctual shadow pass renders each view with
        /// this raw matrix pushed and the viewport placing it in the record's atlas tile, and culls
        /// against the raw (not tile-remapped) frustum. Only [0, PunctualShadowCount) are valid; a
        /// caller's values are overwritten.
        std::array<std::array<mat4, CubeFaceCount>, MaxShadowedPunctual>
            PunctualShadowRawViewProj{};

        /// @brief Resident mesh candidates for this frame; set by the renderer on every Execute.
        ///
        /// The g-buffer pass culls this span against the camera frustum; the shadow pass culls it
        /// against each cascade's light frustum. Borrowed broadphase-cached scratch valid only for
        /// the Execute that produced it. A caller's value is overwritten.
        std::span<const VisibleMesh> Visible;

        /// @brief The renderer's spatial broadphase; set on every Execute.
        ///
        /// A pass queries it (Cull) for the candidate indices its frustum touches; returned ids
        /// index Visible. A caller's value is overwritten.
        const SceneBroadphase* Broadphase = nullptr;

        /// @brief The per-instance skinning palette descriptor set; set by the renderer each Execute.
        ///
        /// Bound by the shadow passes for skinned casters (and the geometry pass for skinned
        /// draws). Holds the same buffer the geometry pass fills in PrepareDraws.
        Ref<DescriptorSet> SkinningPalette;

        /// @brief This frame's PaletteBase per skinned entity (packed Entity → base); set each Execute.
        ///
        /// Filled by the geometry-pass draw preparation; a shadow pass looks up a skinned caster's
        /// palette base here so it casts its posed shadow. Borrowed; valid only for this Execute.
        const unordered_map<u64, u32>* SkinnedPaletteBases = nullptr;
    };

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
        /// Invalidated by Resize and Configure; re-fetch after those calls.
        [[nodiscard]] Ref<ImageView> GetOutput() const;

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
        /// Equals GetFrustumSurvivedCount() — every frustum survivor is a draw under
        /// CullMode::CPU (a materialless or not-yet-resident survivor counts even though it
        /// records no command). The terminal stage of the gathered → frustum-survived → drawn
        /// funnel. Under CullMode::GPU the occlusion stage shows up separately as
        /// GetLastGpuSurvivorCount() (the device-side draws after the hi-Z test zeros occluded
        /// commands). Zero before the first Execute.
        [[nodiscard]] u32 GetLastDrawnCount() const;

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

        /// @brief Returns the per-object velocity target, or null when TAA is off.
        ///
        /// RG screen-space motion vectors written by the velocity prepass. Renderer-owned;
        /// invalidated by Resize and Configure. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetVelocityView() const;

        /// @brief Returns the punctual shadow atlas view (set 1 binding 4).
        ///
        /// A 2D depth atlas of MaxShadowedPunctual·CubeFaceCount tiles, SampleCmp'd by the
        /// lighting pass. Renderer-owned; invalidated by Resize and Configure. Exposed for
        /// the render-pass handoff and for tests inspecting the atlas extent.
        [[nodiscard]] Ref<ImageView> GetPunctualShadowView() const;

    private:
        explicit SceneRenderer(const SceneRendererInfo& info);

        /// @brief Recreates the owned output image and view at the current extent and format.
        void CreateOutput();
        /// @brief Recreates g-buffer images/views at the current extent and (re-)registers them into bindless.
        void CreateGBuffer();
        /// @brief Recreates the hi-Z pyramid image, per-mip views, and reduction descriptor sets.
        ///
        /// Sized to the depth target with a full mip chain; not cleared (it carries data across
        /// frames). Called from CreateGBuffer, after the depth target exists, so the reduction's
        /// mip-0 descriptor set can bind it.
        void CreateHiZ();
        /// @brief Declares the max-Z reduction compute chain into the graph after the tail passes.
        ///
        /// One dispatch per mip: mip 0 reads the depth target, mip n>0 reads hi-Z mip n-1, each
        /// writing its hi-Z mip. The per-mip graph surface derives the chain's read-after-write
        /// barriers.
        /// @param graph  The renderer's internal graph being rebuilt.
        void DeclareHiZReduction(RenderGraph& graph);
        /// @brief Declares the bloom down/up/composite compute sweep into the graph ahead of tonemap.
        ///
        /// Down-sweep (level 0..N-1, barrier between levels), in-place tent up-sweep (level N-2..0,
        /// barrier between levels), then the composite into the result. Per-frame Threshold /
        /// Intensity / Radius ride the compute push, read from the SceneView at record time.
        /// @param graph  The renderer's internal graph being rebuilt.
        void DeclareBloom(RenderGraph& graph);
        /// @brief Recreates the HDR image/view at the current extent and (re-)registers it into bindless.
        void CreateHdr();
        /// @brief Recreates the TAA lit + history targets, or releases them when TAA is off.
        ///
        /// Allocates m_LitImage and m_TaaHistoryImage (both HdrFormat, full extent) and
        /// registers their bindless slots when Settings.TAA is set; otherwise releases any
        /// previously-created ones. Sets m_TaaHistoryReset so the next resolve ignores the
        /// freshly-created history. Called from Create and every Resize/Configure.
        void CreateTaa();
        /// @brief Builds the velocity prepass pipeline once at Create (after the DrawData set layout exists).
        ///
        /// The pipeline shares the per-draw DrawData set and surface push block with the geometry
        /// pass, so it must build after CreateCullResources. Depth-tested, depth-write off.
        void CreateVelocityPipeline();
        /// @brief Recreates the bloom mip-pyramid + result image, their views, and the per-level sets.
        ///
        /// Builds m_BloomImage (one HDR mip chain), the per-mip storage views, the whole-chain
        /// sampled view, the linear sampler, and the per-level down/up + composite descriptor sets,
        /// mirroring CreateHiZ. The result view registers into bindless for the tonemap sample.
        void CreateBloom();
        /// @brief Builds the engine-owned fullscreen pipelines and loads the core PostProcess materials.
        ///
        /// Called once at Create; the lighting pipeline writes the HDR format, the debug-blit pipelines
        /// write the output format.
        void CreatePipelines();

        /// @brief Allocates the directional-shadow set-1 descriptor system once at Create.
        ///
        /// Covers the comparison sampler, the set-1 layout, the descriptor set, the dummy atlas, and
        /// the ShadowConstants ring buffer. All are long-lived past any Configure.
        void CreateShadowSystem();

        /// @brief Clamps m_Settings shadow resolutions to the device-supported maxima.
        ///
        /// Called before any atlas is sized (at construction and before Configure) so an over-large
        /// request degrades to the largest valid atlas rather than a fatal driver error.
        void ClampShadowResolutions();

        /// @brief Recreates the punctual shadow atlas and writes it into set 1 binding 4.
        ///
        /// Sized at PunctualShadowResolution × (MaxShadowedPunctual·CubeFaceCount) tiles.
        /// Called at Create and on every Resize/Configure.
        void CreatePunctualShadowAtlas();

        /// @brief Writes set 1's atlas binding (binding 0) and the debug-blit set's binding 0.
        ///
        /// Supplies either the wired shadow pass's atlas or the dummy when shadows are off.
        /// Called from Rebuild after the pass set is chosen.
        void WriteShadowAtlasBinding(const Ref<ImageView>& atlasView);
        /// @brief Rebuilds the pass set from Settings.Mode and recompiles the RenderGraph.
        void Rebuild();

        /// @brief Sets m_ActiveCull from Settings.Cull and the device-support fallback.
        ///
        /// CullMode::GPU survives only where Context::IsGpuDrivenCullingSupported() is true;
        /// otherwise it degrades to CullMode::CPU. Called at Create and on every Configure.
        void ResolveActiveCullMode();

        /// @brief Declares the GPU occlusion-cull compute pass into the graph (GPU mode only).
        ///
        /// Reads the previous-frame hi-Z and the uploaded candidates, writes each indirect
        /// command's instanceCount (StorageBufferWrite on the indirect import), and the survivor
        /// count. Declared before the geometry pass so the graph derives the
        /// StorageBufferWrite → IndirectRead barrier.
        /// @param graph  The renderer's internal graph being rebuilt.
        void DeclareCullPass(RenderGraph& graph);

        /// @brief Fills the per-draw DrawData buffer (and, under GPU mode, the candidate buffer + groups) for this Execute.
        ///
        /// Computes the camera-frustum survivors, writes the current frame's DrawData region, and
        /// builds the geometry pass's submission plan (m_Internal->Plan). The geometry pass reads
        /// the plan at record time.
        /// @param view                The frame's scene view (broadphase already synced).
        /// @param viewConstantsIndex  This frame's view-constants ring region.
        void PrepareDraws(const SceneView& view, u32 viewConstantsIndex);

        /// @brief Vulkan context for all resource creation.
        Context& m_Context;
        /// @brief Asset manager for engine shader loading.
        AssetManager& m_Assets;
        /// @brief Pixel format of the owned output target.
        Format m_OutputFormat;
        /// @brief Current render extent.
        uvec2 m_Extent;
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

        /// @brief Hi-Z depth pyramid: a max-Z mip chain reduced from the depth target.
        ///
        /// R32Sfloat, sized to the depth target with a full mip chain, Storage | Sampled.
        /// A compute reduction (declared after tonemap) builds it at the end of each
        /// Execute; the occlusion test samples last frame's chain. Persisted across
        /// frames (temporal hi-Z), so it is renderer-owned, not a graph transient, and
        /// not cleared. Recreated on Resize/Configure with the rest of the g-buffer.
        Ref<Image> m_HiZImage;
        /// @brief One storage view per hi-Z mip level (the reduction writes each).
        ///
        /// m_HiZMips[k] views exactly mip k; the reduction's k-th dispatch writes it and
        /// the (k+1)-th reads it, so the per-mip graph surface derives a per-mip barrier.
        vector<Ref<ImageView>> m_HiZMips;
        /// @brief Whole-chain sampled view of the hi-Z pyramid (all mips), registered into bindless.
        Ref<ImageView> m_HiZSampleView;
        /// @brief Bindless slot for the whole-chain sampled hi-Z view.
        TextureHandle m_HiZSampleHandle;

        /// @brief Camera world->clip captured at the end of last Execute (this frame's pyramid pairs with it).
        ///
        /// Identity before the first Execute. The occlusion test pairs the previous-frame
        /// pyramid with this previous-frame matrix (decision 2).
        mat4 m_PreviousViewProj{1.0f};
        /// @brief Last frame's camera state, for the history-validity comparison.
        HiZHistoryState m_PreviousHiZState;
        /// @brief Whether the previous-frame pyramid is valid to occlusion-test against this frame.
        ///
        /// Computed each Execute by IsHiZHistoryValid + the frame-0/post-resize gate; false
        /// until the first Execute.
        bool m_HiZHistoryValid = false;
        /// @brief Set whenever CreateHiZ recreates the pyramid, forcing the next Execute invalid.
        ///
        /// Covers frame 0 (constructed true) and every Resize/Configure (both rebuild the
        /// pyramid through CreateGBuffer → CreateHiZ), so the cleared/new pyramid is never
        /// tested against last frame's matrix.
        bool m_HiZHistoryReset = true;

        /// @brief HDR target the deferred lighting pass writes (linear, unbounded range).
        ///
        /// Renderer-owned and imported like the g-buffer; tonemap maps it to the output format.
        /// Single-copy: one Execute resolves and completes before the next begins, so no
        /// cross-frame ring buffer is needed — the output is consumed in the frame it is written.
        Ref<Image> m_HdrImage;
        /// @brief View over m_HdrImage.
        Ref<ImageView> m_HdrView;

        /// @brief Lighting target when TAA is active (the resolve's current-frame input).
        ///
        /// HdrFormat, full extent. With TAA on, the lighting pass writes here and the TAA
        /// resolve reads it as the current frame; the resolve then writes m_HdrImage, so
        /// bloom and tonemap read the resolved result unchanged. Null when TAA is off
        /// (lighting writes m_HdrImage directly). Recreated on Resize/Configure.
        Ref<Image> m_LitImage;
        /// @brief View over m_LitImage.
        Ref<ImageView> m_LitView;
        /// @brief Bindless slot for the lit view; the resolve samples the current frame through it.
        TextureHandle m_LitHandle;

        /// @brief Persisted previous-frame resolved HDR the TAA resolve reprojects against.
        ///
        /// HdrFormat, full extent, carried across frames (not cleared per frame): the
        /// history-copy pass refreshes it from m_HdrImage at the end of each Execute.
        /// Null when TAA is off. Recreated on Resize/Configure, which invalidates its
        /// contents (m_TaaHistoryReset forces the next resolve to ignore it).
        Ref<Image> m_TaaHistoryImage;
        /// @brief View over m_TaaHistoryImage.
        Ref<ImageView> m_TaaHistoryView;
        /// @brief Bindless slot for the history view; the resolve samples it at the reprojected UV.
        TextureHandle m_TaaHistoryHandle;

        /// @brief Per-object screen-space motion vector target (the velocity prepass writes it).
        ///
        /// RG16Sfloat, full extent. The velocity pass renders geometry with current and
        /// previous transforms into this; the resolve reprojects geometry pixels through it.
        /// Null when TAA is off. Recreated on Resize/Configure alongside the TAA targets.
        Ref<Image> m_VelocityImage;
        /// @brief View over m_VelocityImage.
        Ref<ImageView> m_VelocityView;
        /// @brief Bindless slot for the velocity view; the resolve samples per-object motion through it.
        TextureHandle m_VelocityHandle;

        /// @brief Bloom mip-pyramid image: an HDR mip chain the compute down/up sweep operates on.
        ///
        /// HdrFormat, sized to m_Extent with a mip chain stopping ~3 levels short of 1×1
        /// (Storage | Sampled). The down-sweep produces each coarser level; the up-sweep
        /// accumulates back up. Recreated on Resize/Configure through the retire path.
        Ref<Image> m_BloomImage;
        /// @brief One single-mip storage view per pyramid level (the down/up dispatches write each).
        ///
        /// m_BloomMips[k] views exactly mip k; storage and sampled access to one mip need distinct
        /// views, so the reads go through m_BloomSampleView and the writes through these.
        vector<Ref<ImageView>> m_BloomMips;
        /// @brief Whole-chain sampled view of the bloom pyramid; the down/up dispatches read a level by LOD.
        Ref<ImageView> m_BloomSampleView;
        /// @brief Clamp-to-edge linear sampler for the bilinear down/up taps.
        ///
        /// Off bindless, bound on the bloom compute set 1. Bloom's bilinear taps need linear
        /// filtering (hi-Z's point Load does not), so HdrFormat must advertise
        /// SampledImageFilterLinear — asserted at CreateBloom.
        Ref<Sampler> m_BloomSampler;
        /// @brief Bloom composite result image (full m_Extent); the tonemap samples it when bloom is on.
        Ref<Image> m_BloomResultImage;
        /// @brief View over m_BloomResultImage.
        Ref<ImageView> m_BloomResultView;

        /// @brief Shared sampler fullscreen passes use to sample the g-buffer and HDR target.
        Ref<Sampler> m_Sampler;

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
        /// @brief Bindless slot for the shared sampler.
        SamplerHandle m_SamplerHandle;

        /// @brief Bindless slot for the bloom composite result view; registered in CreateBloom.
        ///
        /// The tonemap samples the composite through this handle when bloom is on. The pyramid
        /// itself (mips + sample view) is bound off bindless on the bloom compute set 1.
        TextureHandle m_BloomResultHandle;

        /// @brief Bindless slot for the bloom pyramid mip 0 view; registered in CreateBloom.
        ///
        /// The DebugView::Bloom arm blits the accumulated bloom contribution through this handle —
        /// pyramid mip 0 after the up-sweep, before composite. Registered alongside the result so
        /// the debug blit reads it like any other bindless source.
        TextureHandle m_BloomMip0Handle;

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

        /// @brief TAA resolve pipeline (reproject + neighborhood-clip + blend), writing HdrFormat.
        Ref<class GraphicsPipeline> m_TaaResolvePipeline;
        /// @brief Layout for m_TaaResolvePipeline: the resolve push block (no extra sets).
        Ref<class PipelineLayout> m_TaaResolveLayout;
        /// @brief TAA history-copy pipeline (unclamped passthrough into the history), writing HdrFormat.
        Ref<class GraphicsPipeline> m_TaaCopyPipeline;
        /// @brief Layout for m_TaaCopyPipeline: a texture + sampler push block.
        Ref<class PipelineLayout> m_TaaCopyLayout;

        /// @brief TAA velocity prepass pipeline (per-object motion vectors), writing VelocityFormat.
        ///
        /// Depth-tested against the g-buffer depth (no write) so only the visible surface
        /// writes its motion. Drives the same per-draw DrawData/candidate path as the
        /// geometry pass, so it shares the DrawData set layout and the surface push block.
        Ref<class GraphicsPipeline> m_VelocityPipeline;
        /// @brief Layout for m_VelocityPipeline: set 1 (DrawData SSBO) + the surface push block.
        Ref<class PipelineLayout> m_VelocityLayout;

        /// @brief Skinned velocity pipeline (skins current + previous position via the palette).
        Ref<class GraphicsPipeline> m_VelocitySkinnedPipeline;
        /// @brief Layout for m_VelocitySkinnedPipeline: set 1 (DrawData) + set 2 (palette) + push.
        Ref<class PipelineLayout> m_VelocitySkinnedLayout;

        /// @brief SSAO fullscreen pipeline writing the R8 AO target.
        Ref<class GraphicsPipeline> m_SsaoPipeline;
        /// @brief Layout for the SSAO pipeline.
        Ref<class PipelineLayout> m_SsaoLayout;

        /// @brief Compute pipeline that reduces one depth/hi-Z mip into the next (max-Z).
        ///
        /// Built once at Create from the core pack's hi_z_reduce.comp. One dispatch per
        /// mip; the per-mip descriptor sets bind that mip's source and destination views.
        Ref<class ComputePipeline> m_HiZReducePipeline;
        /// @brief Layout for m_HiZReducePipeline: set 1 (sampled source + storage dest) + push block.
        Ref<class PipelineLayout> m_HiZReduceLayout;
        /// @brief Set-1 layout for the reduction: binding 0 sampled source, binding 1 storage dest.
        ///
        /// Off bindless — a closed producer→consumer reduction needs no global registration, and a
        /// dedicated set sidesteps the set-0 storage-image argument-buffer path on MoltenVK.
        Ref<DescriptorSetLayout> m_HiZReduceSetLayout;
        /// @brief One reduction descriptor set per destination mip, written on Rebuild.
        ///
        /// Set k binds mip k's source (the depth target for k=0, hi-Z mip k-1 otherwise) and
        /// mip k's destination storage view. Recreated whenever the chain is (Resize/Configure).
        vector<Ref<DescriptorSet>> m_HiZReduceSets;

        /// @brief Cod bloom downsample pipeline (bright-pass + Karis on mip 0, 13-tap below).
        Ref<class ComputePipeline> m_BloomDownPipeline;
        /// @brief Cod bloom upsample-accumulate pipeline (3×3 tent into the finer level).
        Ref<class ComputePipeline> m_BloomUpPipeline;
        /// @brief Kawase bloom downsample pipeline (bright-pass + Karis on mip 0, 5-tap below).
        Ref<class ComputePipeline> m_BloomDownKawasePipeline;
        /// @brief Kawase bloom upsample-accumulate pipeline (8-tap bilinear into the finer level).
        Ref<class ComputePipeline> m_BloomUpKawasePipeline;
        /// @brief Bloom composite compute pipeline (hdr + mip0 * Intensity → result).
        Ref<class ComputePipeline> m_BloomCompositePipeline;
        /// @brief Shared layout for the down/up pipelines (the shared down/up set + push block).
        Ref<class PipelineLayout> m_BloomDownUpLayout;
        /// @brief Layout for the composite pipeline (the distinct composite set + push block).
        Ref<class PipelineLayout> m_BloomCompositeLayout;
        /// @brief Set-1 layout shared by down/up: sampled source (0) + linear sampler (1) + storage dest (2).
        ///
        /// Off bindless — the closed bloom chain needs no global registration, and a dedicated set
        /// sidesteps the set-0 storage-image argument-buffer path on MoltenVK.
        Ref<DescriptorSetLayout> m_BloomDownUpSetLayout;
        /// @brief Set-1 layout for composite: two sampled inputs (0,1) + linear sampler (2) + storage dest (3).
        Ref<DescriptorSetLayout> m_BloomCompositeSetLayout;
        /// @brief One downsample set per level k, binding level k's source and destination.
        ///
        /// Set 0 binds the HDR target as the source; set k>0 binds mip k-1. Recreated with the
        /// pyramid (Resize/Configure).
        vector<Ref<DescriptorSet>> m_BloomDownSets;
        /// @brief One upsample set per finer level k, binding the coarser source (k+1) and dest (k).
        ///
        /// Indexed by the destination (finer) level; m_BloomUpSets[k] reads mip k+1 and writes
        /// mip k. Recreated with the pyramid (Resize/Configure).
        vector<Ref<DescriptorSet>> m_BloomUpSets;
        /// @brief Composite set: HDR + bloom mip 0 sampled inputs and the result storage dest.
        Ref<DescriptorSet> m_BloomCompositeSet;

        /// @brief Debug blit for the albedo channel.
        Ref<class GraphicsPipeline> m_AlbedoBlitPipeline;
        Ref<class PipelineLayout> m_AlbedoBlitLayout;
        /// @brief Debug blit for the normal channel.
        Ref<class GraphicsPipeline> m_NormalBlitPipeline;
        Ref<class PipelineLayout> m_NormalBlitLayout;
        /// @brief Debug blit for the depth buffer.
        Ref<class GraphicsPipeline> m_DepthBlitPipeline;
        Ref<class PipelineLayout> m_DepthBlitLayout;

        /// @brief ORM-channel blit shared by the Roughness/Metallic/Occlusion arms.
        ///
        /// The channel select is a push value, not a separate pipeline. Writes the output format.
        Ref<class GraphicsPipeline> m_OrmBlitPipeline;
        Ref<class PipelineLayout> m_OrmBlitLayout;
        /// @brief Debug blit for the SSAO target.
        Ref<class GraphicsPipeline> m_AoBlitPipeline;
        Ref<class PipelineLayout> m_AoBlitLayout;
        /// @brief Debug blit colorizing the per-object velocity target (DebugView::MotionVectors).
        Ref<class GraphicsPipeline> m_MotionBlitPipeline;
        Ref<class PipelineLayout> m_MotionBlitLayout;
        /// @brief Debug blit for the directional shadow atlas.
        Ref<class GraphicsPipeline> m_ShadowBlitPipeline;
        Ref<class PipelineLayout> m_ShadowBlitLayout;

        /// @brief Directional-shadow set-1 descriptor set (both lighting pipelines).
        ///
        /// Binding 0: shadow atlas (sampled image). Binding 1: immutable comparison sampler
        /// (hardware SampleCmp). Binding 2: ShadowConstants dynamic uniform. Long-lived past any
        /// Configure — the layout, comparison sampler, dummy atlas, and dummy ShadowConstants must
        /// exist whenever the layout does. Bindings 0–1 are written once per Create/Resize/Configure;
        /// binding 2 rings per frame-in-flight, region selected by the dynamic offset at bind.
        Ref<DescriptorSetLayout> m_ShadowSetLayout;
        Ref<DescriptorSet> m_ShadowSet;
        /// @brief Immutable hardware comparison sampler for SampleCmp.
        Ref<Sampler> m_ComparisonSampler;

        /// @brief Debug shadow-atlas blit's dedicated descriptor set.
        ///
        /// Set 1 of the shadow-blit pipeline: binding 0 the atlas, binding 1 an ordinary sampler
        /// (raw depth read, not SampleCmp). Separate layout/set so DebugView::Shadows visualizes
        /// stored depth values.
        Ref<DescriptorSetLayout> m_ShadowBlitSetLayout;
        Ref<DescriptorSet> m_ShadowBlitSet;

        /// @brief 1×1 D32 dummy atlas cleared to depth = 1 (full visibility).
        ///
        /// Bound into set 1 binding 0 whenever no shadow pass is wired, so the layout is always
        /// satisfied and a SampleCmp returns full visibility.
        Ref<Image> m_DummyShadowImage;
        /// @brief View over m_DummyShadowImage.
        Ref<ImageView> m_DummyShadowView;

        /// @brief ShadowConstants ring buffer (set 1 binding 2).
        ///
        /// Host-visible, persistently mapped; framesInFlight regions of m_ShadowRingStride bytes
        /// (align_up(sizeof(ShadowConstantsBlock), minUniformBufferOffsetAlignment)). A per-frame
        /// write touches only the current (not-yet-submitted) region; the dynamic offset at bind
        /// selects it.
        Ref<Buffer> m_ShadowConstantsBuffer;
        /// @brief Stride in bytes between ShadowConstants ring regions.
        u32 m_ShadowRingStride = 0;
        /// @brief Number of frames-in-flight the ring is sized for.
        u32 m_FramesInFlight = 0;

        /// @brief Punctual shadow atlas (set 1 binding 4).
        ///
        /// A 2D D32 atlas of MaxShadowedPunctual·CubeFaceCount tiles at PunctualShadowResolution²
        /// (a spot uses one tile, a point six). DepthAttachment | Sampled — the punctual shadow
        /// pass writes per-tile viewports; the lighting pass SampleCmps through the shared
        /// comparison sampler. Off bindless: a comparison-sampled image bars set-0 bindless on
        /// MoltenVK, and a closed producer→consumer resource needs no global registration.
        /// Cleared to depth = 1 at creation so it is always in a valid sampleable layout.
        Ref<Image> m_PunctualShadowImage;
        /// @brief View over m_PunctualShadowImage.
        Ref<ImageView> m_PunctualShadowView;

        /// @brief Per-light punctual shadow records (set 1 binding 3).
        ///
        /// A std140 dynamic uniform ringed for framesInFlight regions of m_PunctualRingStride bytes,
        /// beside the ShadowConstants ring. Host-visible + persistently mapped; a per-frame write
        /// touches only the current (not-yet-submitted) region. Zeroed regions read as "no map"
        /// (every record Params.x type = 0), so a frame with no shadowed lights is fully lit.
        Ref<Buffer> m_PunctualShadowBuffer;
        /// @brief Stride in bytes between punctual ring regions.
        u32 m_PunctualRingStride = 0;

        /// @brief Core tonemap PostProcess material, loaded once at Create.
        ///
        /// The Final chain's terminal PostProcessScenePass drives it (HDR target as the
        /// runtime-bound input; Exposure written per Execute into its param block).
        AssetHandle<Material> m_TonemapMaterial;

        /// @brief Renderer-owned pass units; rebuilt per Settings.Mode on every Rebuild.
        ///
        /// The geometry pass is always first; Mode selects the tail.
        vector<Unique<ScenePass>> m_Passes;

        /// @brief BVH broadphase over resident draw candidates.
        ///
        /// Synced once at the top of Execute; its candidate span is pointed at by
        /// SceneView::Visible and its tree is queried by the g-buffer and shadow passes.
        /// A static scene does not rebuild the tree.
        SceneBroadphase m_Broadphase;

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

        /// @brief Allocates the per-draw and GPU-cull buffers + their descriptor sets.
        ///
        /// Sized to MaxCullCandidates × frames-in-flight. The per-draw DrawData SSBO is used by
        /// both cull modes (the buffer-indexed surface draw); the candidate/indirect/count buffers
        /// and the cull compute pipeline are used only under CullMode::GPU. Called once at Create.
        void CreateCullResources();

        /// @brief Maximum per-submesh candidates a frame's per-draw / cull buffers hold.
        ///
        /// The fixed candidate maximum (decision 2): the indirect buffer covers this many slots,
        /// culled ones no-op. A frame exceeding it is clamped (the overflow submeshes are not
        /// drawn), asserted in a debug build.
        static constexpr u32 MaxCullCandidates = 4096;

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

        /// @brief Maximum bone matrices a single skinned instance contributes to the palette.
        static constexpr u32 MaxBonesPerSkinnedInstance = 256;
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
        /// @brief Previous frame's PaletteBase per skinned entity; the skinned velocity pass skins
        ///        the previous position through it. Swapped from m_PaletteBaseByEntity each frame.
        unordered_map<u64, u32> m_PreviousPaletteBaseByEntity;

        /// @brief Cull compute pipeline (occlusion test → instanceCount), GPU mode only.
        Ref<class ComputePipeline> m_CullPipeline;
        /// @brief Layout for m_CullPipeline: set 1 (hi-Z, candidates, commands, count) + push block.
        Ref<class PipelineLayout> m_CullLayout;
        /// @brief Set-1 layout for the cull pass.
        Ref<DescriptorSetLayout> m_CullSetLayout;
        /// @brief Descriptor set for the cull pass, written on CreateCullResources / CreateGBuffer.
        Ref<DescriptorSet> m_CullSet;

        /// @brief Uploaded camera-frustum survivors (world bounds + draw args), GPU mode only.
        ///
        /// Host-visible, ring-buffered; one CullCandidate record per survivor this frame.
        Ref<Buffer> m_CullCandidateBuffer;
        /// @brief Indirect command buffer the cull writes and the geometry pass reads.
        ///
        /// Device-local, Storage | Indirect | TransferSrc, ring-buffered. The compute pass writes
        /// each VkDrawIndexedIndirectCommand's instanceCount; the geometry pass issues it.
        Ref<Buffer> m_IndirectBuffer;
        /// @brief GPU survivor-count buffer the cull atomically increments, read back for the stat.
        ///
        /// Host-visible, Storage | TransferSrc, ring-buffered; one u32 per frame region, zeroed
        /// before the cull dispatch and read one frame late.
        Ref<Buffer> m_CullCountBuffer;
        /// @brief Imported buffer id for the indirect command buffer in the internal graph.
        ResourceId m_IndirectId;

        /// @brief Previous-frame camera world->clip the cull pass screen-bounds candidates with.
        ///
        /// The pyramid is last frame's depth, so the cull must project with last frame's matrix.
        mat4 m_CullPrevViewProj{1.0f};
        /// @brief mip-0 pixel extent of the hi-Z pyramid the cull samples.
        uvec2 m_CullHiZExtent{};
        /// @brief Candidate count, region base, count-buffer slot, and history flag the cull dispatch reads.
        ///
        /// Filled by PrepareDraws each GPU Execute and read by the cull pass at record time.
        u32 m_CullCandidateCount = 0;
        /// @brief Candidate/command region base (currentFrame * MaxCullCandidates).
        u32 m_CullFrameBase = 0;
        /// @brief This frame's slot in the survivor-count buffer.
        u32 m_CullCountIndex = 0;
        /// @brief 1 when the previous-frame pyramid is valid to occlude against, else 0 (frustum-only).
        u32 m_CullHistoryValid = 0;
        /// @brief Reused per-frame frustum-survivor candidate ids (broadphase Cull scratch).
        vector<u32> m_CullScratch;

        /// @brief The cull mode in effect after the device-support fallback (CPU if GPU unsupported).
        SceneRendererSettings::CullMode m_ActiveCull = SceneRendererSettings::CullMode::CPU;
        /// @brief Set once the GPU-unsupported fallback has logged, so the WARN fires only once.
        bool m_GpuCullWarned = false;
        /// @brief Survivor count read back from the previous GPU Execute (the debug stat).
        mutable u32 m_LastGpuSurvivorCount = 0;
        /// @brief Number of candidate slots the GPU cull wrote this Execute (for the readback span).
        u32 m_GpuCandidateCount = 0;
        /// @brief The frame-in-flight region the previous GPU Execute wrote, for the count/flag readback.
        u32 m_GpuReadbackRegion = 0;
        /// @brief True once a GPU Execute has filled a region to read back.
        bool m_GpuReadbackValid = false;

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
        /// @brief Imported id for the velocity target under TAA.
        ResourceId m_VelocityId;
        /// @brief Per-mip subresource handle for the bloom pyramid the down/up sweep reads and writes.
        MipChainId m_BloomChainId;
        /// @brief Imported id for the bloom composite result.
        ResourceId m_BloomResultId;
        /// @brief Imported id for the directional shadow atlas.
        ResourceId m_ShadowId;
        /// @brief Imported id for the SSAO target.
        ResourceId m_SsaoId;
        /// @brief Imported id for the final output target.
        ResourceId m_OutputId;
        /// @brief Imported id for the depth source the reduction reads into hi-Z mip 0.
        ResourceId m_HiZDepthSourceId;
        /// @brief Per-mip subresource handle for the hi-Z chain the reduction writes.
        MipChainId m_HiZChainId;

        /// @brief True when the last Rebuild wired the bloom chain (Final mode + Settings.Bloom).
        ///
        /// Execute binds the bloom imports and writes the bloom params only when true.
        bool m_BloomActive = false;

        /// @brief True when the last Rebuild wired the TAA passes (Final mode + Settings.TAA).
        ///
        /// Execute jitters the projection, binds the lit/history imports, and pushes the
        /// resolve's history-validity flag only when true.
        bool m_TaaActive = false;

        /// @brief True when the last Rebuild wired the velocity prepass (Final mode + Settings.TAA).
        ///
        /// Execute binds the velocity import and maintains the previous-transform maps only when true.
        bool m_VelocityActive = false;

        /// @brief Forces the next resolve to ignore history (frame 0 and after Resize/Configure).
        ///
        /// The history image holds undefined or stale-extent content after creation, so the
        /// resolve must fall back to the current color until one frame has populated it. Set
        /// by CreateTaa, cleared after each Execute, mirroring m_HiZHistoryReset.
        bool m_TaaHistoryReset = true;

        /// @brief Monotonic frame counter driving the Halton jitter sequence.
        ///
        /// Incremented every Execute (independent of the TAA toggle so toggling on does not
        /// snap the sequence). Folds into TaaJitterSampleCount.
        u64 m_FrameIndex = 0;

        /// @brief Previous frame's world matrix per entity, keyed by a packed Entity id.
        ///
        /// The velocity prepass needs each drawn object's prior transform; PrepareDraws looks
        /// it up here and writes it into the per-draw record. An entity absent (first seen, or
        /// after a TAA toggle) reprojects with zero object motion. Maintained only while TAA is
        /// active; swapped from m_CurrentWorlds at the end of each Execute.
        unordered_map<u64, mat4> m_PreviousWorlds;
        /// @brief This frame's world matrix per entity; swapped into m_PreviousWorlds after Execute.
        unordered_map<u64, mat4> m_CurrentWorlds;

        /// @brief True when the last Rebuild wired the directional shadow pass (Final mode + Settings.Shadows).
        ///
        /// Execute binds the shadow import and writes the light-space matrix only when true.
        bool m_ShadowActive = false;

        /// @brief Non-owning pointer to the wired ShadowScenePass, or null when shadows are compiled out.
        ///
        /// The renderer reads its produced atlas view to thread into PassIO and to bind the shadow
        /// import per Execute. The pass outlives this pointer (m_Passes is cleared and rebuilt together).
        ShadowScenePass* m_ShadowPass = nullptr;

        /// @brief True when the last Rebuild wired the punctual shadow pass.
        ///
        /// Execute binds the punctual atlas import only when true. The pass renders the
        /// renderer-owned punctual atlas (m_PunctualShadowView).
        bool m_PunctualShadowActive = false;
        /// @brief Imported id for the punctual shadow atlas.
        ResourceId m_PunctualShadowId;
        /// @brief Non-owning pointer to the wired punctual shadow pass.
        PunctualShadowScenePass* m_PunctualShadowPass = nullptr;

        /// @brief True when the last Rebuild wired the SSAO pass (Final mode + Settings.AO).
        ///
        /// Execute binds the AO import only when true.
        bool m_SsaoActive = false;
        /// @brief Non-owning pointer into m_Passes to the SsaoScenePass; null when AO is off.
        class SsaoScenePass* m_SsaoPass = nullptr;

        /// @brief Opaque compiled graph; replayed every Execute.
        ///
        /// Held behind an opaque pointer so this header stays free of the full CompiledGraph type.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
