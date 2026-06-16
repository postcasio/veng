#pragma once

#include <Veng/Veng.h>
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
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ScenePass;
    class Image;
    class Sampler;

    // Topology/sizing knobs. A change here is a Configure → recompile: a knob that
    // turns a pass on/off or re-wires the pass set lives here, not in SceneView.
    struct SceneRendererSettings
    {
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
    // Light is a per-frame VALUE, not a borrowed reference: the renderer selects
    // the scene's directional light into it on every Execute (the first Light
    // entity, or a documented zero-intensity default when the scene has none).
    struct SceneView
    {
        const Scene& World;
        const Camera& Camera;
        Veng::Light Light;
        f32 Delta = 0.0f;
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
        [[nodiscard]] Ref<ImageView> GetDepthView() const;

        // The HDR target the deferred lighting pass writes (before the tail
        // pass maps it to the output). Exposed for tests and tooling.
        [[nodiscard]] Ref<ImageView> GetHdrView() const;

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
        // Build the engine-owned lighting + HDR-blit pipelines once at Create.
        void CreatePipelines();
        // Rebuild the RenderGraph from the pass units and re-Compile().
        void Rebuild();

        Context& m_Context;
        AssetManager& m_Assets;
        Format m_OutputFormat;
        uvec2 m_Extent;
        SceneRendererSettings m_Settings;

        Ref<Image> m_OutputImage;
        Ref<ImageView> m_OutputView;

        // The g-buffer targets: G0 albedo, G1 world-normal, depth. Renderer-owned
        // (sampled downstream, so not graph transients) and imported.
        Ref<Image> m_AlbedoImage;
        Ref<ImageView> m_AlbedoView;
        Ref<Image> m_NormalImage;
        Ref<ImageView> m_NormalView;
        Ref<Image> m_DepthImage;
        Ref<ImageView> m_DepthView;

        // The HDR target the deferred lighting pass writes (linear, unbounded
        // range) and the tail pass samples. Renderer-owned and imported like the
        // g-buffer; tonemap maps it to the output format.
        Ref<Image> m_HdrImage;
        Ref<ImageView> m_HdrView;

        // The shared sampler a fullscreen pass samples the g-buffer/HDR through.
        Ref<Sampler> m_Sampler;

        // Bindless slots for the g-buffer/HDR views + the sampler, registered once
        // at Create and re-registered on Resize (the old slots released through the
        // per-frame retire window).
        TextureHandle m_AlbedoHandle;
        TextureHandle m_NormalHandle;
        TextureHandle m_DepthHandle;
        TextureHandle m_HdrHandle;
        SamplerHandle m_SamplerHandle;

        // The engine-owned deferred-lighting pipeline + layout (g-buffer + light →
        // HDR) and the HDR-blit pipeline + layout (HDR → output). Built once from
        // the core pack's shaders; the lighting pipeline writes the HDR format, the
        // blit pipeline the output format.
        Ref<class GraphicsPipeline> m_LightingPipeline;
        Ref<class PipelineLayout> m_LightingLayout;
        Ref<class GraphicsPipeline> m_HdrBlitPipeline;
        Ref<class PipelineLayout> m_HdrBlitLayout;

        // The renderer owns its pass units in fixed wiring order and walks them on
        // every rebuild.
        vector<Unique<ScenePass>> m_Passes;

        // The imported ids every rebuild re-declares, bound to their concrete
        // views per Execute and threaded to the pass units through PassIO.
        ResourceId m_AlbedoId;
        ResourceId m_NormalId;
        ResourceId m_DepthId;
        ResourceId m_HdrId;
        ResourceId m_OutputId;

        // Compiled once per Create/Resize/Configure, replayed every Execute. The
        // concrete type is RenderGraph's CompiledGraph; held by an opaque pointer so
        // this header stays free of the full RenderGraph definition.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
