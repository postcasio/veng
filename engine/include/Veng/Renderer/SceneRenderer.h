#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>

#include <Veng/Scene/Camera.h>

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
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ScenePass;

    // Topology/sizing knobs. A change here is a Configure → recompile: a knob that
    // turns a pass on/off or re-wires the pass set lives here, not in SceneView.
    struct SceneRendererSettings
    {
    };

    struct SceneRendererInfo
    {
        Context& Context;
        Format OutputFormat = Format::Undefined; // a format, not a caller-owned target
        uvec2 Extent = {};
        SceneRendererSettings Settings;
    };

    // Per-frame input. Not owned by the renderer and shared across N renderers:
    // World/Camera are borrowed references. Fields are named for their role.
    struct SceneView
    {
        const Scene& World;
        const Camera& Camera;
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

    private:
        explicit SceneRenderer(const SceneRendererInfo& info);

        // Recreate the owned output image/view at the current extent/format.
        void CreateOutput();
        // Rebuild the RenderGraph from the pass units and re-Compile().
        void Rebuild();

        Context& m_Context;
        Format m_OutputFormat;
        uvec2 m_Extent;
        SceneRendererSettings m_Settings;

        Ref<Image> m_OutputImage;
        Ref<ImageView> m_OutputView;

        // The renderer owns its pass units in fixed wiring order and walks them on
        // every rebuild.
        vector<Unique<ScenePass>> m_Passes;

        // The imported-output id every rebuild re-declares, threaded to the pass
        // units through PassIO.
        ResourceId m_OutputId;

        // Compiled once per Create/Resize/Configure, replayed every Execute. The
        // concrete type is RenderGraph's CompiledGraph; held by an opaque pointer so
        // this header stays free of the full RenderGraph definition.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
