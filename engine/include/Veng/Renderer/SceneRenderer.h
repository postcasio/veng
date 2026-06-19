#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>

// A long-lived, configurable render pipeline that owns an offscreen target,
// renders a Scene from a Camera through an internal compiled RenderGraph composed
// of reusable ScenePass units, and hands back a sampleable result.
//
// Its surface is a lifetime split keyed on how often each piece of state changes:
// Create allocates persistent resources and compiles the graph; Resize recreates
// extent-sized resources and recompiles; Configure recreates affected resources
// and recompiles topology; Execute replays the graph against a per-frame SceneView
// and never reallocates or recompiles; GetOutput returns the owned result.
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
    class Image;
    class Sampler;

    // Which result the renderer produces, re-wiring the pass set: Final is the full
    // deferred chain (g-buffer → lighting → tonemap), the others terminate the chain
    // after the g-buffer with a single fullscreen debug blit of one g-buffer channel.
    // Changing it is a genuine topology change driven through Configure → recompile.
    enum class DebugView : u8 { Final, Albedo, Normal, Depth };

    // Topology/sizing knobs. A change here is a Configure → recompile: a knob that
    // turns a pass on/off or re-wires the pass set lives here, not in SceneView.
    struct SceneRendererSettings
    {
        // Re-wires the pass set (a topology change → Configure → recompile).
        DebugView Mode = DebugView::Final;

        // The tonemap pass's exposure scale, applied before the tone curve. A
        // recompile-safe value (it never changes topology) kept a setting so the
        // Settings surface is exercised; a purely per-frame knob could instead ride
        // SceneView (the per-frame-vs-recompile distinction).
        f32 Exposure = 1.0f;

        // Whether the bloom post chain runs ahead of tonemap. A topology change: it
        // inserts/removes the four bloom stages (bright-pass → blur H → blur V →
        // composite), so it drives a Configure → recompile. The bloom Threshold and
        // Intensity are per-frame values on SceneView, not settings — they tune the
        // effect without a recompile, the split the plumbing-vs-effect line draws.
        bool Bloom = true;
    };

    struct SceneRendererInfo
    {
        Context& Context;
        // The renderer's passes load their engine shaders (the fullscreen blit,
        // the lighting pass) from the core pack through this manager; it must
        // outlive the renderer.
        AssetManager& Assets;
        Format OutputFormat = Format::Undefined; // a format, not a caller-owned target
        uvec2 Extent = {};
        SceneRendererSettings Settings;
    };

    // Per-frame input. Not owned by the renderer and shared across N renderers:
    // World/Camera are borrowed references. Fields are named for their role.
    //
    // The renderer reads the scene's lights itself: on every Execute it walks
    // View<Transform, Light> up to MaxLights, packs each into the ring-buffered
    // light buffer, and the lighting pass loops over the live count. A scene with
    // no Light renders flat-ambient (the loop runs zero times).
    struct SceneView
    {
        const Scene& World;
        const Camera& Camera;
        f32 Delta = 0.0f;

        // The live light count this frame, set by the renderer on every Execute
        // (the number of (Transform, Light) entities it packed, capped at
        // MaxLights). The lighting pass loops [0, LightCount). A caller's value is
        // overwritten — the renderer owns this selection.
        u32 LightCount = 0;

        // The fixed cap on lights the renderer packs per frame; the lighting pass
        // evaluates the full BRDF per light up to this count.
        static constexpr u32 MaxLights = BindlessRegistry::MaxLights;

        // The bloom bright-pass luminance knee and the composite mix — per-frame
        // values written into the bloom materials' ring-buffered param blocks each
        // Execute (the same stall-free path Exposure uses). Tuning them never
        // recompiles; only the Bloom on/off topology toggle does. Ignored when the
        // bloom chain is inactive.
        f32 BloomThreshold = 1.0f;
        f32 BloomIntensity = 1.0f;
    };

    class SceneRenderer
    {
    public:
        static Unique<SceneRenderer> Create(const SceneRendererInfo& info);
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Recreate the extent-sized output through the deferred-destruction retire
        // path and rebuild + recompile the internal graph. Invalidates the Ref a
        // prior GetOutput() returned — a consumer caching a bindless TextureHandle
        // or ImGui texture from it must re-fetch and re-register after this.
        void Resize(uvec2 extent);

        // Recreate only affected resources and recompile the graph's topology.
        // Invalidates the prior GetOutput() Ref like Resize.
        void Configure(const SceneRendererSettings& settings);

        // Replay the internal graph against this frame's view, recording each pass
        // unit's draws. Never reallocates or recompiles.
        void Execute(CommandBuffer& cmd, const SceneView& view);

        // The sampleable view of the owned result. Invalidated by Resize/Configure.
        [[nodiscard]] Ref<ImageView> GetOutput() const;

        // The deferred g-buffer the geometry pass writes — the sampleable views
        // and their bindless slots. Renderer-owned and imported into the internal
        // graph; recreated and re-registered on Resize. Exposed for tests and
        // tooling that inspect the intermediate targets; a normal consumer reads
        // only GetOutput().
        [[nodiscard]] Ref<ImageView> GetAlbedoView() const;
        [[nodiscard]] Ref<ImageView> GetNormalView() const;
        [[nodiscard]] Ref<ImageView> GetOrmView() const;
        [[nodiscard]] Ref<ImageView> GetDepthView() const;

        // The HDR target the deferred lighting pass writes (before the tail
        // pass maps it to the output). Exposed for tests and tooling.
        [[nodiscard]] Ref<ImageView> GetHdrView() const;

        // The bloom composite result the tonemap stage reads when Bloom is on (the
        // HDR target plus the blurred bright residual). Null when Bloom is off — the
        // tonemap stage then reads the raw HDR target. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetBloomResultView() const;

    private:
        explicit SceneRenderer(const SceneRendererInfo& info);

        // Recreate the owned output image/view at the current extent/format.
        void CreateOutput();
        // Recreate the g-buffer images/views at the current extent and (re-)register
        // them and the shared sampler into the bindless registry.
        void CreateGBuffer();
        // Recreate the HDR image/view at the current extent and (re-)register it
        // into the bindless registry.
        void CreateHdr();
        // Recreate the four bloom intermediate images/views at the current extent
        // and (re-)register them into the bindless registry. The bright residual,
        // the two separable-blur ping-pong targets, and the composite result are
        // renderer-owned and imported (a later stage samples the prior stage's output
        // through bindless, which needs a Ref<ImageView> — a transient cannot be
        // registered). Recreated through the deferred Release() path on
        // Resize/Configure like the g-buffer/HDR targets.
        void CreateBloom();
        // Build the engine-owned fullscreen pipelines (lighting and the
        // albedo/normal/depth debug blits) and load the core tonemap PostProcess
        // material once at Create.
        void CreatePipelines();
        // Rebuild the pass set from Settings.Mode and the RenderGraph from it, then
        // re-Compile().
        void Rebuild();

        Context& m_Context;
        AssetManager& m_Assets;
        Format m_OutputFormat;
        uvec2 m_Extent;
        SceneRendererSettings m_Settings;

        Ref<Image> m_OutputImage;
        Ref<ImageView> m_OutputView;

        // The g-buffer targets: G0 albedo, G1 world-normal, G2 packed ORM, depth.
        // Renderer-owned (sampled downstream, so not graph transients) and imported.
        Ref<Image> m_AlbedoImage;
        Ref<ImageView> m_AlbedoView;
        Ref<Image> m_NormalImage;
        Ref<ImageView> m_NormalView;
        Ref<Image> m_OrmImage;
        Ref<ImageView> m_OrmView;
        Ref<Image> m_DepthImage;
        Ref<ImageView> m_DepthView;

        // The HDR target the deferred lighting pass writes (linear, unbounded
        // range) and the tonemap pass samples. Renderer-owned and imported like the
        // g-buffer; tonemap maps it to the output format.
        //
        // Single-in-flight contract: every renderer-owned image above (g-buffer,
        // depth, HDR, output) is single-copy. One Execute resolves and completes
        // before the next begins; within a frame, written-then-read images are
        // correctly ordered by the graph's derived barriers, and the retire path
        // covers destruction safety on Resize/Configure. There is no cross-frame ring
        // buffer because the output is consumed in the frame it is written — a
        // compositor samples GetOutput() for the same frame the renderer wrote it, so
        // the frames-in-flight > 1 hazard (a compositor caching/sampling an output
        // across frames-in-flight) does not arise.
        Ref<Image> m_HdrImage;
        Ref<ImageView> m_HdrView;

        // The bloom chain's renderer-owned intermediates (linear HDR-format, same
        // single-copy contract as the g-buffer/HDR targets): the bright-pass residual,
        // the two separable-blur ping-pong targets, and the composite result the
        // tonemap stage reads. Imported into the internal graph; a later stage samples
        // the prior stage's output through its bindless handle, so each needs a
        // Ref<ImageView> (a graph transient cannot be registered into bindless).
        Ref<Image> m_BloomBrightImage;
        Ref<ImageView> m_BloomBrightView;
        Ref<Image> m_BloomBlurHImage;
        Ref<ImageView> m_BloomBlurHView;
        Ref<Image> m_BloomBlurVImage;
        Ref<ImageView> m_BloomBlurVView;
        Ref<Image> m_BloomResultImage;
        Ref<ImageView> m_BloomResultView;

        // The shared sampler a fullscreen pass samples the g-buffer/HDR through.
        Ref<Sampler> m_Sampler;

        // Bindless slots for the g-buffer/HDR views + the sampler, registered once
        // at Create and re-registered on Resize (the old slots released through the
        // per-frame retire window).
        TextureHandle m_AlbedoHandle;
        TextureHandle m_NormalHandle;
        TextureHandle m_OrmHandle;
        TextureHandle m_DepthHandle;
        TextureHandle m_HdrHandle;
        SamplerHandle m_SamplerHandle;

        // Bindless slots for the bloom intermediates, registered with them in
        // CreateBloom and released through the per-frame retire window on recreate.
        TextureHandle m_BloomBrightHandle;
        TextureHandle m_BloomBlurHHandle;
        TextureHandle m_BloomBlurVHandle;
        TextureHandle m_BloomResultHandle;

        // The engine-owned fullscreen pipelines + layouts, all built once at Create
        // from the core pack's shaders. The lighting pipeline writes the HDR format;
        // the three debug-blit pipelines (albedo/normal/depth) write the output
        // format. The pass set Configure wires from Mode references the pipelines it
        // needs; the rest stay built but unused.
        Ref<class GraphicsPipeline> m_LightingPipeline;
        Ref<class PipelineLayout> m_LightingLayout;
        Ref<class GraphicsPipeline> m_AlbedoBlitPipeline;
        Ref<class PipelineLayout> m_AlbedoBlitLayout;
        Ref<class GraphicsPipeline> m_NormalBlitPipeline;
        Ref<class PipelineLayout> m_NormalBlitLayout;
        Ref<class GraphicsPipeline> m_DepthBlitPipeline;
        Ref<class PipelineLayout> m_DepthBlitLayout;

        // The core tonemap PostProcess material, loaded once at Create. The Final
        // chain's terminal PostProcessScenePass drives it (HDR target as the
        // runtime-bound input, exposure written per Execute into its param block).
        AssetHandle<Material> m_TonemapMaterial;

        // The core bloom PostProcess materials, loaded once at Create. The bloom
        // chain (when Settings.Bloom) drives four PostProcessScenePass stages over
        // them ahead of tonemap: bright-pass → blur H → blur V → composite. The two
        // blur stages reuse one shader through two materials differing only in the
        // cooked Horizontal axis param. Threshold (bright-pass) and Intensity
        // (composite) are written per Execute into their ring-buffered blocks.
        AssetHandle<Material> m_BloomBrightMaterial;
        AssetHandle<Material> m_BloomBlurHMaterial;
        AssetHandle<Material> m_BloomBlurVMaterial;
        AssetHandle<Material> m_BloomCompositeMaterial;

        // The renderer owns its pass units; the set is rebuilt per Settings.Mode on
        // every Rebuild (the geometry pass is always first; Mode selects the tail).
        vector<Unique<ScenePass>> m_Passes;

        // The imported ids every rebuild re-declares, bound to their concrete
        // views per Execute and threaded to the pass units through PassIO.
        ResourceId m_AlbedoId;
        ResourceId m_NormalId;
        ResourceId m_OrmId;
        ResourceId m_DepthId;
        ResourceId m_HdrId;
        ResourceId m_BloomBrightId;
        ResourceId m_BloomBlurHId;
        ResourceId m_BloomBlurVId;
        ResourceId m_BloomResultId;
        ResourceId m_OutputId;

        // Whether the last Rebuild wired the bloom chain (Final mode + Settings.Bloom).
        // Execute binds the bloom imports and writes the bloom params only then.
        bool m_BloomActive = false;

        // Compiled once per Create/Resize/Configure, replayed every Execute. The
        // concrete type is RenderGraph's CompiledGraph; held by an opaque pointer so
        // this header stays free of the full RenderGraph definition.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
