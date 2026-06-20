#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
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
    };

    /// @brief Topology and sizing knobs for SceneRenderer.
    ///
    /// A change to any field here is a Configure → recompile. Knobs that turn a pass
    /// on/off or re-wire the pass set live here; per-frame values belong on SceneView.
    struct SceneRendererSettings
    {
        /// @brief Selects which result the renderer produces; re-wires the pass set on change.
        DebugView Mode = DebugView::Final;

        /// @brief Exposure scale applied before the tone curve.
        ///
        /// Recompile-safe: it never changes topology, so it could ride SceneView, but
        /// lives here to exercise the Settings surface.
        f32 Exposure = 1.0f;

        /// @brief Whether the bloom post chain runs ahead of tonemap.
        ///
        /// A topology change: it inserts/removes the four bloom stages (bright-pass →
        /// blur H → blur V → composite). BloomThreshold and BloomIntensity are
        /// per-frame values on SceneView and do not trigger a recompile.
        bool Bloom = true;

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
        const Camera& Camera;
        /// @brief Frame delta time in seconds.
        f32 Delta = 0.0f;

        /// @brief Live light count this frame; set by the renderer on every Execute.
        ///
        /// The number of (Transform, Light) entities packed, capped at MaxLights. The
        /// lighting pass loops [0, LightCount). A caller's value is overwritten.
        u32 LightCount = 0;

        /// @brief Maximum number of lights the renderer packs per frame.
        static constexpr u32 MaxLights = BindlessRegistry::MaxLights;

        /// @brief Bloom bright-pass luminance knee; written into the bloom material's param block each Execute.
        ///
        /// Tuning this does not trigger a recompile. Ignored when the bloom chain is inactive.
        f32 BloomThreshold = 1.0f;
        /// @brief Bloom composite mix intensity; written into the bloom material's param block each Execute.
        ///
        /// Tuning this does not trigger a recompile. Ignored when the bloom chain is inactive.
        f32 BloomIntensity = 1.0f;

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
        /// One per per-submesh candidate the g-buffer pass processed after the cull; always
        /// <= GetLastVisibleCount(). A materialless submesh still counts as a survivor.
        /// Zero before the first Execute.
        [[nodiscard]] u32 GetLastDrawnCount() const;

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

        /// @brief Returns the HDR target the deferred lighting pass writes before tonemap.
        ///
        /// Exposed for tests and tooling. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetHdrView() const;

        /// @brief Returns the bloom composite result the tonemap stage reads when Bloom is on.
        ///
        /// Null when Bloom is off (tonemap reads the raw HDR target instead). Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetBloomResultView() const;

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
        /// @brief Recreates the HDR image/view at the current extent and (re-)registers it into bindless.
        void CreateHdr();
        /// @brief Recreates the four bloom intermediate images/views and registers them into bindless.
        ///
        /// The bright residual, two separable-blur ping-pong targets, and the composite result are
        /// renderer-owned and imported; a later stage samples the prior stage's output through its
        /// bindless handle, so each needs a Ref<ImageView> (a graph transient cannot be registered).
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

        /// @brief HDR target the deferred lighting pass writes (linear, unbounded range).
        ///
        /// Renderer-owned and imported like the g-buffer; tonemap maps it to the output format.
        /// Single-copy: one Execute resolves and completes before the next begins, so no
        /// cross-frame ring buffer is needed — the output is consumed in the frame it is written.
        Ref<Image> m_HdrImage;
        /// @brief View over m_HdrImage.
        Ref<ImageView> m_HdrView;

        /// @brief Bloom chain intermediates (linear HDR-format, same single-copy contract).
        ///
        /// Bright-pass residual, two separable-blur ping-pong targets, and the composite result.
        /// Imported into the internal graph; each needs a Ref<ImageView> so it can be registered
        /// into the bindless set (a graph transient cannot be registered).
        Ref<Image> m_BloomBrightImage;
        /// @brief Bright-pass residual view.
        Ref<ImageView> m_BloomBrightView;
        /// @brief Horizontal blur ping-pong image.
        Ref<Image> m_BloomBlurHImage;
        /// @brief Horizontal blur view.
        Ref<ImageView> m_BloomBlurHView;
        /// @brief Vertical blur ping-pong image.
        Ref<Image> m_BloomBlurVImage;
        /// @brief Vertical blur view.
        Ref<ImageView> m_BloomBlurVView;
        /// @brief Bloom composite result image.
        Ref<Image> m_BloomResultImage;
        /// @brief Bloom composite result view.
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

        /// @brief Bindless slots for the bloom intermediates; registered in CreateBloom.
        TextureHandle m_BloomBrightHandle;
        /// @brief Bindless slot for the horizontal blur view.
        TextureHandle m_BloomBlurHHandle;
        /// @brief Bindless slot for the vertical blur view.
        TextureHandle m_BloomBlurVHandle;
        /// @brief Bindless slot for the bloom composite view.
        TextureHandle m_BloomResultHandle;

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

        /// @brief SSAO fullscreen pipeline writing the R8 AO target.
        Ref<class GraphicsPipeline> m_SsaoPipeline;
        /// @brief Layout for the SSAO pipeline.
        Ref<class PipelineLayout> m_SsaoLayout;

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

        /// @brief Core bloom PostProcess materials, loaded once at Create.
        ///
        /// The bloom chain drives four PostProcessScenePass stages ahead of tonemap:
        /// bright-pass → blur H → blur V → composite. The two blur stages reuse one shader
        /// through two materials differing only in the cooked Horizontal axis param.
        /// BloomThreshold and BloomIntensity are written per Execute into their ring-buffered blocks.
        AssetHandle<Material> m_BloomBrightMaterial;
        /// @brief Horizontal blur material.
        AssetHandle<Material> m_BloomBlurHMaterial;
        /// @brief Vertical blur material.
        AssetHandle<Material> m_BloomBlurVMaterial;
        /// @brief Bloom composite material.
        AssetHandle<Material> m_BloomCompositeMaterial;

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

        /// @brief Non-owning pointer to the g-buffer pass's drawn-mesh counter.
        ///
        /// Set on every Rebuild (the pass owns the u32 through m_Passes). Null before the
        /// first Rebuild. Raw pointer rather than a typed back-reference because
        /// GBufferScenePass is a .cpp-local type.
        const u32* m_GBufferDrawnCount = nullptr;

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
        /// @brief Imported id for the bright-pass residual.
        ResourceId m_BloomBrightId;
        /// @brief Imported id for the horizontal blur target.
        ResourceId m_BloomBlurHId;
        /// @brief Imported id for the vertical blur target.
        ResourceId m_BloomBlurVId;
        /// @brief Imported id for the bloom composite result.
        ResourceId m_BloomResultId;
        /// @brief Imported id for the directional shadow atlas.
        ResourceId m_ShadowId;
        /// @brief Imported id for the SSAO target.
        ResourceId m_SsaoId;
        /// @brief Imported id for the final output target.
        ResourceId m_OutputId;

        /// @brief True when the last Rebuild wired the bloom chain (Final mode + Settings.Bloom).
        ///
        /// Execute binds the bloom imports and writes the bloom params only when true.
        bool m_BloomActive = false;

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
